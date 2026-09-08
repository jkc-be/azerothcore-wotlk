/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "bridge/Service.h"
#include "gtest/gtest.h"
#include <boost/asio.hpp>
#include <filesystem>
#include <thread>
#include <unistd.h>

namespace Alles::Bridge
{
TEST(AllesRuntimePolicyBridge, ConcurrentActorsRetainSlotsUntilDeliveryAndCancelledInterviewsCannotApply)
{
    auto const base = std::filesystem::temp_directory_path() / ("alles-agent-slots-" + std::to_string(getpid()));
    std::filesystem::create_directories(base);
    Settings settings{0, "worker-secret", "profile", "model", (base / "ledger").string(), 0, 30};
    settings.controlToken = "control-secret";
    settings.run = "run-one";
    settings.policy.concurrentJobs = settings.concurrentCalls = 2;
    ActorStore store;
    Interpreter::PilotCoordinator coordinator(store);
    {
        Service service(coordinator, settings);
        ActorKey const first{ActorKind::Player, 1}, second{ActorKind::Player, 2};
        uint64_t sample = 0;
        service.SetInterviewContext([&](ActorKey actor) -> std::optional<boost::json::object>
        {
            if (actor != first)
                return std::nullopt;
            return boost::json::object{{"sample", ++sample}, {"intentions", boost::json::object{}}};
        });
        uint64_t const now = 100000;
        bool paused = false;
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        socket.connect({boost::asio::ip::address_v4::loopback(), service.IO().Port()});
        socket.non_blocking(true);
        uint64_t sequence = 0;
        auto call = [&](std::string const& op, boost::json::object args, std::string token = "worker-secret")
        {
            auto text = boost::json::serialize(boost::json::object{{"id", std::to_string(++sequence)},
                {"token", token}, {"op", op}, {"args", args}}) + '\n';
            boost::asio::write(socket, boost::asio::buffer(text));
            std::string response;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (response.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                service.Update(900000, now, paused, now);
                char bytes[65536];
                boost::system::error_code error;
                auto count = socket.read_some(boost::asio::buffer(bytes), error);
                if (!error)
                    response.append(bytes, count);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return Parse(response).as_object().at("result").as_object();
        };
        auto hello = call("worker_hello", {{"profile", "profile"}, {"model", "model"}, {"maxInFlight", 2},
            {"timeoutSeconds", 20}, {"planningVersion", 1}, {"contractVersion", 2}, {"maxCallsPerJob", 1}});
        EXPECT_EQ(Number(hello, "contractVersion"), 2u);
        ASSERT_TRUE(service.QueueConversation("first", {}, now, first));
        ASSERT_TRUE(service.QueueConversation("second", {}, now, second));
        auto receipt = call("interview_submit", {{"run", settings.run}, {"jobToken", "interview-one"},
            {"owner", boost::json::object{{"kind", 0}, {"id", 1}}},
            {"context", boost::json::object{{"character", "first"}, {"place", nullptr}, {"asked", "now"},
                {"memories", boost::json::array{}}, {"history", boost::json::array{}}, {"message", "What next?"},
                {"retrieval", boost::json::object{}}}}}, "control-secret");
        ASSERT_EQ(String(receipt, "status"), "queued");
        auto next = [&](std::string const& purpose)
        {
            boost::json::object answer;
            for (unsigned tries = 0; tries < 100; ++tries)
            {
                answer = call("next_jobs", {{"n", 1}});
                if (answer.contains(purpose))
                    return answer.at(purpose).as_object();
            }
            return boost::json::object{};
        };
        auto a = next("conversation"), b = next("conversation");
        ASSERT_EQ(String(a, "jobToken"), "first");
        ASSERT_EQ(String(b, "jobToken"), "second");
        EXPECT_EQ(Number(service.Status(), "activeJobs"), 2u);
        auto report = [&](boost::json::object const& job, boost::json::object response)
        {
            return boost::json::object{{"jobToken", job.at("jobToken")}, {"permitId", job.at("permitId")},
                {"outcome", "success"}, {"response", std::move(response)}, {"promptTokens", 10},
                {"completionTokens", 5}, {"latencyMs", 20}, {"callCount", 1}, {"usageKnown", true}};
        };
        boost::json::object silent{{"reply", false}, {"text", ""}, {"action", "none"}};
        paused = true;
        EXPECT_EQ(String(call("submit_conversation", report(a, silent)), "status"), "accepted");
        service.CancelConversation("second");
        EXPECT_EQ(String(call("submit_conversation", report(b, silent)), "status"), "stale");
        EXPECT_EQ(Number(service.Status(), "activeJobs"), 1u); // First result still awaits its owning runtime.
        EXPECT_TRUE(call("next_jobs", {{"n", 1}}).at("job").is_null());
        paused = false;
        EXPECT_TRUE(call("next_jobs", {{"n", 1}}).at("job").is_null()); // Same actor remains occupied.
        ASSERT_EQ(service.TakeConversations().size(), 1u);
        auto interview = next("interview");
        ASSERT_EQ(String(interview, "jobToken"), "interview-one");
        EXPECT_EQ(Number(interview.at("context").as_object().at("personalState").as_object(), "sample"), 2u);
        service.CancelActor(first);
        EXPECT_EQ(String(call("submit_interview", report(interview, {{"text", "I do not know yet."}})),
            "status"), "stale");
        EXPECT_EQ(String(call("interview_status", {{"run", settings.run}, {"jobToken", "interview-one"}},
            "control-secret"), "status"), "cancelled");
        EXPECT_EQ(Number(service.Status(), "usedRequests"), 3u);
        EXPECT_EQ(Number(service.Status(), "backendCalls"), 3u);
        EXPECT_EQ(Number(service.Status(), "activeJobs"), 0u);
        EXPECT_TRUE(service.TakeConversations().empty());
        EXPECT_TRUE(service.TakePlanning().empty());
        EXPECT_EQ(coordinator.Stats().modelMemories, 0u);
    }
    std::filesystem::remove_all(base);
}

TEST(AllesRuntimePolicyBridge, DedicatedCapabilityPersistsAndFencesCommandsWithoutResettingUsage)
{
    auto const base = std::filesystem::temp_directory_path() / ("alles-runtime-policy-" + std::to_string(getpid()));
    std::filesystem::create_directories(base);
    Settings settings{0, "worker-secret", "profile", "model", (base / "ledger").string(), 0, 30};
    settings.controlToken = "control-secret";
    settings.policyFile = (base / "policy.json").string();
    settings.run = "run-one";
    ActorStore store;
    Interpreter::PilotCoordinator coordinator(store);
    {
        Service service(coordinator, settings);
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket(io);
        socket.connect({boost::asio::ip::address_v4::loopback(), service.IO().Port()});
        socket.non_blocking(true);
        uint64_t sequence = 0;
        auto call = [&](std::string const& op, boost::json::object args, std::string token = "control-secret")
        {
            auto text = boost::json::serialize(boost::json::object{{"id", std::to_string(++sequence)},
                {"token", token}, {"op", op}, {"args", args}}) + '\n';
            boost::asio::write(socket, boost::asio::buffer(text));
            std::string response;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (response.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                service.Update(900000, 100000, true, 100000); // Controls remain alive during a paused game clock.
                char bytes[65536];
                boost::system::error_code error;
                auto count = socket.read_some(boost::asio::buffer(bytes), error);
                if (!error)
                    response.append(bytes, count);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return Parse(response).as_object();
        };
        EXPECT_TRUE(call("interpreter_status", {}, "worker-secret").contains("error"));
        EXPECT_TRUE(call("worker_hello", {{"profile", "profile"}, {"model", "model"},
            {"maxInFlight", 1}, {"timeoutSeconds", 20}}).contains("error"));
        boost::json::object policy{{"schema", 1}, {"mode", "unlimited"}, {"modelRpm", 30},
            {"concurrentJobs", 2}, {"concurrentCalls", 2}, {"waitingPerActor", 2}, {"waitingGlobal", 10},
            {"bytesPerActor", 49152}, {"bytesGlobal", 245760}, {"maxWaitMs", 20000}, {"trialMaxCalls", 100}};
        boost::json::object command{{"run", settings.run}, {"command", "change-one"}, {"expectedRevision", 1},
            {"policy", policy}};
        auto receipt = call("interpreter_policy", command).at("result").as_object();
        EXPECT_TRUE(receipt.at("pending").as_bool());
        EXPECT_EQ(Number(receipt, "revision"), 1u);
        for (unsigned tries = 0; tries < 100 && Number(service.Status().at("policy").as_object(), "revision") == 1;
            ++tries)
            call("interpreter_status", {});
        auto active = service.Status().at("policy").as_object();
        EXPECT_EQ(Number(active, "revision"), 2u);
        EXPECT_EQ(String(active, "mode"), "unlimited");
        EXPECT_EQ(Number(service.Status(), "usedRequests"), 0u);
        EXPECT_FALSE(call("interpreter_policy", command).at("result").as_object().at("pending").as_bool());
        command["command"] = "stale";
        EXPECT_TRUE(call("interpreter_policy", command).at("result").as_object().contains("error"));
        command["expectedRevision"] = 2;
        command["run"] = "old-run";
        EXPECT_TRUE(call("interpreter_policy", command).at("result").as_object().contains("error"));
        command["run"] = settings.run;
        command.at("policy").as_object()["concurrentJobs"] = 0;
        EXPECT_TRUE(call("interpreter_policy", command).at("result").as_object().contains("error"));
        EXPECT_EQ(Number(service.Status().at("policy").as_object(), "revision"), 2u);
    }
    {
        Service restored(coordinator, settings);
        auto active = restored.Status().at("policy").as_object();
        EXPECT_EQ(Number(active, "revision"), 2u);
        EXPECT_EQ(String(active, "mode"), "unlimited");
        EXPECT_EQ(Number(active, "concurrentJobs"), 2u);
    }
    std::filesystem::remove_all(base);
}
}
