/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
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
TEST(AllesConversationBridgeTest, DurableSharedRateBudgetRefillsAndRejectsInvalidOrLateActions)
{
    auto path = std::filesystem::temp_directory_path() / ("alles-chat-test-" + std::to_string(getpid()));
    std::filesystem::remove(path);
    ActorStore store;
    Interpreter::PilotCoordinator coordinator(store);
    uint64_t now = 100000;
    {
        Service service(coordinator, {0, "secret", "profile", "model", path.string(), 0, 2});
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        socket.connect({boost::asio::ip::address_v4::loopback(), service.IO().Port()});
        socket.non_blocking(true);
        uint64_t sequence = 0;
        auto call = [&](std::string op, boost::json::object args)
        {
            auto bytes = boost::json::serialize(boost::json::object{
                             {"id", std::to_string(++sequence)}, {"token", "secret"}, {"op", op}, {"args", args}}) +
                         '\n';
            boost::asio::write(socket, boost::asio::buffer(bytes));
            std::string response;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (response.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                service.Update(now, now);
                char data[16384];
                boost::system::error_code error;
                auto count = socket.read_some(boost::asio::buffer(data), error);
                if (!error)
                    response.append(data, count);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return Parse(response).as_object();
        };
        call("worker_hello", {{"profile", "profile"}, {"model", "model"}, {"maxInFlight", 1}, {"timeoutSeconds", 20}});
        auto claim = [&](std::string const& id, bool enqueue = true)
        {
            if (enqueue)
                EXPECT_TRUE(service.QueueConversation(id, {{"message", "Could you lend a hand?"}}, now));
            boost::json::object answer;
            for (unsigned tries = 0; tries < 100; ++tries)
            {
                answer = call("next_jobs", {{"n", 1}}).at("result").as_object();
                if (answer.contains("conversation"))
                    return answer.at("conversation").as_object();
            }
            return boost::json::object{};
        };
        auto submit = [&](boost::json::object const& job, boost::json::object reply)
        {
            return call("submit_conversation", {{"jobToken", job.at("jobToken")},
                                                {"permitId", job.at("permitId")},
                                                {"response", reply},
                                                {"outcome", "success"},
                                                {"promptTokens", 10},
                                                {"completionTokens", 10},
                                                {"latencyMs", 1}});
        };
        auto job = claim("first");
        ASSERT_TRUE(job.contains("permitId"));
        EXPECT_EQ(Number(service.Status(), "usedRequests"), 1u);
        EXPECT_TRUE(call("next_jobs", {{"n", 1}}).at("result").as_object().at("job").is_null());
        EXPECT_TRUE(submit(job, {{"reply", true}, {"text", "hello"}, {"action", "teleport"}}).contains("error"));
        EXPECT_TRUE(submit(job, {{"reply", true}, {"text", " .server shutdown 1"},
            {"action", "none"}}).contains("error"));
        EXPECT_TRUE(submit(job, {{"reply", true}, {"text", "|Hitem:1|h"}, {"action", "none"}}).contains("error"));
        EXPECT_EQ(String(submit(job, {{"reply", true}, {"text", "What do you need?"}, {"action", "none"}})
                             .at("result")
                             .as_object(),
                         "status"),
                  "accepted");
        auto results = service.TakeConversations();
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(String(results.front().response, "text"), "What do you need?");
        EXPECT_EQ(
            String(submit(job, {{"reply", true}, {"text", "duplicate"}, {"action", "follow"}}).at("result").as_object(),
                   "status"),
            "stale");
        job = claim("second");
        ASSERT_TRUE(job.contains("permitId"));
        EXPECT_EQ(String(submit(job, {{"reply", false}, {"text", ""}, {"action", "none"}}).at("result").as_object(),
                         "status"),
                  "accepted");
        EXPECT_TRUE(call("next_jobs", {{"n", 1}}).at("result").as_object().at("budgetExhausted").as_bool());
        EXPECT_EQ(Number(service.Status(), "remainingRequests"), 0u);
        now += 60000;
        // Sustained conversation traffic must yield a slot to an actual waiting memory job.
        ActorKey const owner{ActorKind::Player, 1};
        auto generation = store.Activate(owner, 1);
        OwnerSnapshot snapshot;
        snapshot.owner = owner;
        ASSERT_TRUE(store.FinishLoad(owner, *generation, snapshot, now - 5000, now - 5000));
        ASSERT_TRUE(coordinator.Track(owner));
        Perception observation;
        observation.text = "I heard about work outside the valley.";
        observation.source.name = "Speaker";
        observation.gameTimeMs = now - 5000;
        observation.admittedRealTimeMs = now - 5000;
        ASSERT_TRUE(store.Observe(owner, observation, now - 5000));
        coordinator.Update(now, now);
        ASSERT_TRUE(service.QueueConversation("refilled", {{"message", "Could you lend a hand?"}}, now));
        auto memory = call("next_jobs", {{"n", 1}}).at("result").as_object().at("job");
        ASSERT_TRUE(memory.is_object());
        auto const& memoryJob = memory.as_object();
        EXPECT_TRUE(call("release_job", {{"jobToken", memoryJob.at("jobToken")},
            {"leaseGeneration", memoryJob.at("leaseGeneration")}, {"requestId", "fairness"}})
                .at("result").as_object().at("ok").as_bool());
        job = claim("refilled", false);
        ASSERT_TRUE(job.contains("permitId"));
        EXPECT_EQ(Number(service.Status(), "usedRequests"), 3u);
        EXPECT_EQ(Number(service.Status(), "remainingRequests"), 1u);
        now += 25001;
        EXPECT_EQ(
            String(submit(job, {{"reply", true}, {"text", "too late"}, {"action", "assist"}}).at("result").as_object(),
                   "status"),
            "stale");
    }
    {
        Transport reopened(0, path.string(), "profile", 0);
        EXPECT_EQ(reopened.Charged(), 3u);
        EXPECT_EQ(reopened.RecentReservations().back(), 160000u);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".lock");
}
} // namespace Alles::Bridge

#include "runtime/ConversationRouting.h"

TEST(AllesConversationRoutingTest, DirectAddressDoesNotTurnMentionsIntoTriggerWords)
{
    std::vector<std::string> names{"Humana", "Humanb"};
    EXPECT_EQ(Alles::AddressedBot("hUmAnA, would you come with me?", names), "Humana");
    EXPECT_EQ(Alles::AddressedBot("Hey Humanb tell me about yourself", names), "Humanb");
    EXPECT_FALSE(Alles::AddressedBot("I saw Humana nearby", names));
    EXPECT_FALSE(Alles::AddressedBot("Humanab said hello", names));
    EXPECT_FALSE(Alles::AddressedBot("could someone lend a hand", names));
    EXPECT_FALSE(Alles::AddressedBot("HELP", names));
}

TEST(AllesConversationLedgerTest, CompactionPreservesCountRecentReservationsAndLock)
{
    using namespace Alles::Bridge;
    auto path = std::filesystem::temp_directory_path() / ("alles-checkpoint-test-" + std::to_string(getpid()));
    {
        std::ofstream output(path);
        for (unsigned i = 0; i < 3000; ++i)
            output << boost::json::serialize(boost::json::object{{"profile", "profile"},
                {"reservedUnixMs", 1}, {"padding", std::string(150, 'x')}}) << '\n';
    }
    {
        Transport transport(0, path.string(), "profile", 0);
        EXPECT_EQ(transport.Charged(), 3000u);
        transport.Reserve("last", "{\"profile\":\"profile\",\"permit\":\"last\",\"reservedUnixMs\":100000}");
        std::vector<Frame> frames;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (frames.empty() && std::chrono::steady_clock::now() < deadline)
        {
            frames = transport.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(frames.size(), 1u);
        EXPECT_TRUE(Parse(frames.front().text).as_object().at("ok").as_bool());
        EXPECT_LT(std::filesystem::file_size(path), 1024u);
        EXPECT_THROW(Transport(0, path.string(), "profile", 0), std::exception);
    }
    {
        Transport transport(0, path.string(), "profile", 0);
        EXPECT_EQ(transport.Charged(), 3001u);
        EXPECT_EQ(transport.RecentReservations(), std::vector<uint64_t>{100000});
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".lock");
}
