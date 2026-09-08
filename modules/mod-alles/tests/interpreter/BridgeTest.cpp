/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "bridge/Service.h"
#include "gtest/gtest.h"
#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace Alles::Bridge
{
TEST(AllesWireTest, RejectsAmbiguousDocuments)
{
    for (auto text : {"{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}", "{}{}", "{\"x\":NaN}", "{\"x\":\"\\ud800\"}"})
        EXPECT_THROW(Parse(text), std::exception);
    auto value = Parse("{\"id\":18446744073709551615,\"nested\":[{\"a\":1},{\"a\":2}]}");
    EXPECT_EQ(Number(value.as_object(), "id"), UINT64_MAX);
    EXPECT_THROW(Number(Parse("{\"id\":1.5}").as_object(), "id"), std::exception);
    EXPECT_THROW(Parse(std::string(65537, ' ')), std::exception);
}

TEST(AllesTransportTest, DurableReservationSurvivesRestartAndPinsProfile)
{
    auto path = std::filesystem::temp_directory_path() / ("alles-ledger-test-" + std::to_string(getpid()));
    std::filesystem::remove(path);
    {
        Transport transport(0, path.string(), "profile", 1);
        EXPECT_EQ(transport.Charged(), 0u);
        transport.Reserve("permit", "{\"profile\":\"profile\",\"permit\":\"permit\"}");
        std::vector<Frame> frames;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (frames.empty() && std::chrono::steady_clock::now() < deadline)
        {
            frames = transport.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(frames.size(), 1u);
        EXPECT_EQ(frames.front().connection, 0u);
        EXPECT_TRUE(Parse(frames.front().text).as_object().at("ok").as_bool());
    }
    {
        Transport transport(0, path.string(), "profile", 1);
        EXPECT_EQ(transport.Charged(), 1u);
        EXPECT_THROW(Transport(0, path.string(), "profile", 1), std::exception);
    }
    EXPECT_THROW(Transport(0, path.string(), "different", 1), std::exception);
    std::filesystem::remove(path);
}

TEST(AllesTransportTest, ReadsPipelinedFramesAndSerializesOutOfOrderReplies)
{
    auto path = std::filesystem::temp_directory_path() / ("alles-wire-test-" + std::to_string(getpid()));
    std::filesystem::remove(path);
    {
        Transport transport(0, path.string(), "profile", 1);
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        socket.connect({boost::asio::ip::address_v4::loopback(), transport.Port()});
        std::string input = "{\"id\":1}\n{\"id\":2}\n";
        boost::asio::write(socket, boost::asio::buffer(input));
        std::vector<Frame> received;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (received.size() < 2 && std::chrono::steady_clock::now() < deadline)
        {
            for (auto& frame : transport.Poll())
                received.push_back(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(received.size(), 2u);
        transport.Reply(received[1].connection, "{\"id\":2}");
        transport.Reply(received[0].connection, "{\"id\":1}");
        socket.non_blocking(true);
        std::string output;
        while (std::count(output.begin(), output.end(), '\n') < 2 && std::chrono::steady_clock::now() < deadline)
        {
            char bytes[256];
            boost::system::error_code error;
            auto size = socket.read_some(boost::asio::buffer(bytes), error);
            if (!error)
                output.append(bytes, size);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(output, "{\"id\":2}\n{\"id\":1}\n");
    }
    std::filesystem::remove(path);
}
TEST(AllesBridgeTest, AuthenticatedPermitIsDurableIdempotentAndBudgeted)
{
    auto path = std::filesystem::temp_directory_path() / ("alles-service-test-" + std::to_string(getpid()));
    std::filesystem::remove(path);
    ActorStore store;
    Interpreter::PilotCoordinator coordinator(store);
    ActorKey owner{ActorKind::Player, 1};
    auto generation = store.Activate(owner, 1);
    OwnerSnapshot snapshot;
    snapshot.owner = owner;
    ASSERT_TRUE(store.FinishLoad(owner, *generation, snapshot, 0, 0));
    ASSERT_TRUE(coordinator.Track(owner));
    Perception perception;
    perception.text = "A wolf was wounded.";
    perception.source.name = "Speaker";
    ASSERT_TRUE(store.Observe(owner, perception, 0));
    {
        Service service(coordinator, {0, "secret", "profile", "model", path.string(), 1});
        coordinator.Update(5000, 5000);
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        socket.connect({boost::asio::ip::address_v4::loopback(), service.IO().Port()});
        socket.non_blocking(true);
        uint64_t sequence = 0;
        auto call = [&](std::string op, boost::json::object args, std::string token = "secret")
        {
            auto bytes = boost::json::serialize(boost::json::object{
                             {"id", std::to_string(++sequence)}, {"token", token}, {"op", op}, {"args", args}}) +
                         '\n';
            boost::asio::write(socket, boost::asio::buffer(bytes));
            std::string response;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (response.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                service.Update(5000, 5000);
                char data[65536];
                boost::system::error_code error;
                auto count = socket.read_some(boost::asio::buffer(data), error);
                if (!error)
                    response.append(data, count);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return Parse(response).as_object();
        };
        boost::json::object hello{
            {"profile", "profile"}, {"model", "model"}, {"maxInFlight", 1}, {"timeoutSeconds", 20}};
        EXPECT_TRUE(call("worker_hello", hello, "wrong").contains("error"));
        auto welcomed = call("worker_hello", hello).at("result").as_object();
        auto worker = String(welcomed, "workerId");
        auto offered = call("next_jobs", {{"n", 1}}).at("result").as_object().at("job").as_object();
        auto job = coordinator.Inspect(owner);
        ASSERT_TRUE(job);
        boost::json::object args{
            {"jobToken", job->jobToken}, {"leaseGeneration", job->leaseGeneration}, {"requestId", "request-1"}};
        auto receipt = call("begin_attempt", args).at("result").as_object();
        for (unsigned retry = 0; String(receipt, "status") == "pending" && retry < 100; ++retry)
            receipt = call("begin_attempt", args).at("result").as_object();
        ASSERT_EQ(String(receipt, "status"), "granted");
        auto duplicate = call("begin_attempt", args).at("result").as_object();
        EXPECT_EQ(String(receipt, "permitId"), String(duplicate, "permitId"));
        EXPECT_EQ(Number(service.Status(), "usedRequests"), 1u);
        args["requestId"] = "different";
        EXPECT_TRUE(call("begin_attempt", args).contains("error"));
        auto proposal = offered.at("draft").as_object();
        for (auto key : {"bootEpoch", "jobToken", "ownerKind", "ownerId", "actorGeneration", "workerId",
                         "profileFingerprint", "leaseGeneration"})
            proposal[key] = offered.at(key);
        proposal["permitId"] = receipt.at("permitId");
        auto applied = call("submit_result", {{"proposal", proposal}}).at("result").as_object();
        EXPECT_EQ(String(applied, "status"), "applied");
        EXPECT_EQ(store.FindReady(owner)->memories.front().formation, FormationMode::Model);
        EXPECT_EQ(String(call("submit_result", {{"proposal", proposal}}).at("result").as_object(), "status"),
                  "applied");
        EXPECT_EQ(store.FindReady(owner)->memories.size(), 1u);
        EXPECT_TRUE(call("next_jobs", {{"n", 1}}).at("result").as_object().at("budgetExhausted").as_bool());
    }
    std::filesystem::remove(path);
}

} // namespace Alles::Bridge

namespace Alles::Bridge
{
TEST(AllesTransportTest, ContinuousReservationCountDoesNotWrapAtTheOldTrialCounterBoundary)
{
    auto const path = std::filesystem::temp_directory_path() / ("alles-counter-test-" + std::to_string(getpid()));
    {
        std::ofstream file(path);
        file << "{\"profile\":\"profile\",\"charged\":4294967296,\"recentBuckets\":[]}\n";
        file << "{\"profile\":\"profile\",\"permit\":\"next\"}\n";
    }
    {
        Transport transport(0, path.string(), "profile", 0);
        EXPECT_EQ(transport.Charged(), uint64_t(UINT32_MAX) + 2);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".lock");
}

TEST(AllesRateWindow, HighThroughputUsesBoundedCountsAndRealWindowSurvivesRestart)
{
    ReservationHistory history;
    for (uint64_t i = 0; i < 100000; ++i)
        AddReservation(history, 100001 + i / 100);
    EXPECT_LE(history.size(), 2u);
    EXPECT_EQ(RecentCount(history, 160000), 100000u);
    EXPECT_EQ(RecentCount(history, 162000), 0u);
    EXPECT_EQ(RecentCount(history, 1), 100000u); // A wall-clock correction cannot refund newer reservations.
}

TEST(AllesTransportTest, ManagedPolicyIsAtomicAndReportsPersistenceFailure)
{
    auto base = std::filesystem::temp_directory_path() / ("alles-policy-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(base);
    auto ledger = base / "ledger";
    auto policy = base / "policy.json";
    {
        Transport transport(0, ledger.string(), "profile", 0);
        ASSERT_TRUE(transport.PersistPolicy(policy.string(), "first", "{\"schema\":1,\"revision\":2}"));
        auto wait = [&]
        {
            std::vector<Frame> frames;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (frames.empty() && std::chrono::steady_clock::now() < deadline)
            {
                frames = transport.Poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return frames;
        };
        auto frames = wait();
        ASSERT_EQ(frames.size(), 1u);
        EXPECT_TRUE(Parse(frames[0].text).as_object().at("ok").as_bool());
        EXPECT_TRUE(std::filesystem::exists(policy));
        ASSERT_TRUE(transport.PersistPolicy((base / "missing" / "policy.json").string(), "second", "{}"));
        frames = wait();
        ASSERT_EQ(frames.size(), 1u);
        EXPECT_FALSE(Parse(frames[0].text).as_object().at("ok").as_bool());
        EXPECT_TRUE(std::filesystem::exists(policy));
    }
    std::filesystem::remove_all(base);
}
}
