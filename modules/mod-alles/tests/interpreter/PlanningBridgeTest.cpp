/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "bridge/PlanningWire.h"
#include "bridge/Service.h"
#include "gtest/gtest.h"
#include <boost/asio.hpp>
#include <filesystem>
#include <thread>
#include <unistd.h>

namespace Alles::Bridge
{
namespace
{
boost::json::object Decision()
{
    return {{"version", 1}, {"capability", "investigate_report"}, {"quest", 0}, {"place", 12},
        {"person", nullptr}, {"evidence", "reply"}, {"reason", "A speaker reports possible work."}};
}

class AllesPlanningBridgeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        path = std::filesystem::temp_directory_path() / ("alles-plan-test-" + std::to_string(getpid()));
        std::filesystem::remove(path);
        service = std::make_unique<Service>(coordinator, Settings{0, "secret", "profile", "model", path.string(), 20});
        for (auto* socket : {&first, &second})
        {
            socket->connect({boost::asio::ip::address_v4::loopback(), service->IO().Port()});
            socket->non_blocking(true);
        }
    }

    void TearDown() override
    {
        service.reset();
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".lock");
    }

    boost::json::object Call(std::string const& op, boost::json::object args, bool other = false)
    {
        auto& socket = other ? second : first;
        auto bytes = boost::json::serialize(boost::json::object{{"id", std::to_string(++sequence)},
            {"token", "secret"}, {"op", op}, {"args", std::move(args)}}) + '\n';
        boost::asio::write(socket, boost::asio::buffer(bytes));
        std::string response;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (response.find('\n') == std::string::npos && std::chrono::steady_clock::now() < deadline)
        {
            service->Update(now, now);
            char data[16384];
            boost::system::error_code error;
            auto count = socket.read_some(boost::asio::buffer(data), error);
            if (!error)
                response.append(data, count);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return Parse(response).as_object();
    }

    boost::json::object Hello(bool planning, bool other = false, uint32_t version = 1)
    {
        boost::json::object args{{"profile", "profile"}, {"model", "model"},
            {"maxInFlight", 1}, {"timeoutSeconds", 20}};
        if (planning)
            args["planningVersion"] = version;
        return Call("worker_hello", args, other);
    }

    boost::json::object Claim(char const* purpose)
    {
        for (unsigned tries = 0; tries < 100; ++tries)
        {
            auto answer = Call("next_jobs", {{"n", 1}}).at("result").as_object();
            if (answer.contains(purpose) && answer.at(purpose).is_object())
                return answer.at(purpose).as_object();
        }
        return {};
    }

    boost::json::object Submit(boost::json::object const& job, std::string purpose = "planning",
        bool other = false, std::string outcome = "success")
    {
        boost::json::object response = purpose == "planning" ? Decision() :
            boost::json::object{{"reply", false}, {"text", ""}, {"action", "none"}};
        return Call("submit_" + purpose, {{"jobToken", job.at("jobToken")}, {"permitId", job.at("permitId")},
            {"response", std::move(response)}, {"outcome", outcome}, {"promptTokens", 10},
            {"completionTokens", 10}, {"latencyMs", 1}}, other);
    }

    ActorStore store;
    Interpreter::PilotCoordinator coordinator{store};
    std::unique_ptr<Service> service;
    std::filesystem::path path;
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket first{io}, second{io};
    uint64_t now = 100000;
    uint64_t sequence = 0;
};
}

TEST(AllesPlanningWireTest, ClosedResponseCannotReplaceIssuedActorOrObjectiveFence)
{
    CapabilityContext issued{{ActorKind::Player, 1}, 7, 9, 11, true, {}, {12}, {}};
    auto response = Decision();
    auto decision = DecodePlanningDecision(response, issued);
    EXPECT_EQ(decision.request.owner, issued.owner);
    EXPECT_EQ(decision.request.actorGeneration, 7u);
    EXPECT_EQ(decision.request.objective, 9u);
    EXPECT_EQ(decision.request.revision, 11u);
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({"investigate_report", false, true, false,
        "a heard lead", "investigate", "handoff"}));
    EXPECT_TRUE(registry.Validate(decision.request, issued).empty());
    ++issued.revision;
    EXPECT_EQ(registry.Validate(decision.request, issued), "stale_objective_or_actor");
    response["owner"] = 999;
    EXPECT_THROW(DecodePlanningDecision(response, issued), std::exception);
    response = Decision();
    response["version"] = 2;
    EXPECT_THROW(DecodePlanningDecision(response, issued), std::exception);
    response = Decision();
    response["place"] = uint64_t(1) << 32;
    EXPECT_THROW(DecodePlanningDecision(response, issued), std::exception);
    response = Decision();
    response.erase("reason");
    EXPECT_THROW(DecodePlanningDecision(response, issued), std::exception);
    response = Decision();
    response["person"] = boost::json::object{{"kind", 255}, {"id", 1}};
    EXPECT_THROW(DecodePlanningDecision(response, issued), std::exception);
    response = Decision();
    response["reason"] = std::string(513, 'x');
    EXPECT_THROW(DecodePlanningDecision(response, issued), std::exception);
}

TEST_F(AllesPlanningBridgeTest, NegotiatesVersionAndFencesPurposeConnectionPermitAndCancellation)
{
    ASSERT_TRUE(Hello(false).contains("result"));
    ASSERT_TRUE(service->QueuePlanning("plan", {{"purpose", "advice"}}, now));
    EXPECT_FALSE(service->QueueConversation("plan", {}, now));
    EXPECT_TRUE(Call("next_jobs", {{"n", 1}}).at("result").as_object().at("job").is_null());
    EXPECT_EQ(Number(service->Status(), "usedRequests"), 0u);
    EXPECT_TRUE(Hello(true, false, 2).contains("error"));
    ASSERT_TRUE(Hello(true).contains("result"));
    ASSERT_TRUE(Hello(true, true).contains("result"));
    auto job = Claim("planning");
    ASSERT_TRUE(job.contains("permitId"));
    EXPECT_EQ(Number(job, "contractVersion"), 1u);
    EXPECT_EQ(String(Submit(job, "conversation").at("result").as_object(), "status"), "stale");
    EXPECT_EQ(String(Submit(job, "planning", true).at("result").as_object(), "status"), "stale");
    auto forged = job;
    forged["permitId"] = "forged";
    EXPECT_EQ(String(Submit(forged).at("result").as_object(), "status"), "stale");
    EXPECT_EQ(String(Submit(job).at("result").as_object(), "status"), "accepted");
    EXPECT_EQ(service->TakePlanning().size(), 1u);
    EXPECT_TRUE(service->TakeConversations().empty());
    EXPECT_EQ(String(Submit(job).at("result").as_object(), "status"), "stale");
    ASSERT_TRUE(service->QueuePlanning("cancelled", {}, now));
    job = Claim("planning");
    ASSERT_TRUE(job.contains("permitId"));
    service->CancelPlanning("cancelled");
    EXPECT_EQ(String(Submit(job).at("result").as_object(), "status"), "stale");
    EXPECT_TRUE(service->TakePlanning().empty());
    now += 25001;
    service->Update(now, now);
    EXPECT_EQ(Number(service->Status(), "usedRequests"), 2u);
    EXPECT_EQ(Number(service->Status(), "planningQueued"), 0u);
}

TEST_F(AllesPlanningBridgeTest, MemoryPlanningAndChatEachGetATurnAndFailuresRetainTheSlot)
{
    ASSERT_TRUE(Hello(true).contains("result"));
    ActorKey const owner{ActorKind::Player, 1};
    auto generation = store.Activate(owner, 1);
    OwnerSnapshot snapshot;
    snapshot.owner = owner;
    ASSERT_TRUE(store.FinishLoad(owner, *generation, snapshot, now - 5000, now - 5000));
    ASSERT_TRUE(coordinator.Track(owner));
    Perception perception;
    perception.text = "A speaker offered advice.";
    perception.source.name = "Speaker";
    perception.gameTimeMs = now - 5000;
    perception.admittedRealTimeMs = now - 5000;
    ASSERT_TRUE(store.Observe(owner, perception, now - 5000));
    coordinator.Update(now, now);
    ASSERT_TRUE(service->QueuePlanning("plan", {}, now));
    ASSERT_TRUE(service->QueueConversation("chat", {}, now));
    auto memory = Claim("job");
    ASSERT_TRUE(memory.contains("leaseGeneration"));
    ASSERT_TRUE(Call("release_job", {{"jobToken", memory.at("jobToken")},
        {"leaseGeneration", memory.at("leaseGeneration")}, {"requestId", "fairness"}})
        .at("result").as_object().at("ok").as_bool());
    auto planning = Claim("planning");
    ASSERT_TRUE(planning.contains("permitId"));
    EXPECT_EQ(String(Submit(planning).at("result").as_object(), "status"), "accepted");
    auto chat = Claim("conversation");
    ASSERT_TRUE(chat.contains("permitId"));
    EXPECT_EQ(String(Submit(chat, "conversation").at("result").as_object(), "status"), "accepted");
    ASSERT_TRUE(service->QueuePlanning("failure", {}, now));
    // Remove the released memory candidate so the next assertion isolates the occupied HTTP slot.
    coordinator.Forget(owner);
    planning = Claim("planning");
    ASSERT_TRUE(planning.contains("permitId"));
    EXPECT_EQ(String(Submit(planning, "planning", false, "failed").at("result").as_object(), "status"), "accepted");
    ASSERT_TRUE(service->QueueConversation("waiting", {}, now));
    now += 5001;
    EXPECT_TRUE(Call("next_jobs", {{"n", 1}}).at("result").as_object().at("job").is_null());
    now += 20000;
    chat = Claim("conversation");
    // The original queued chat now has less than the required full provider window: no late admission.
    EXPECT_TRUE(chat.empty());
    EXPECT_EQ(Number(service->Status(), "planningFailed"), 1u);
    EXPECT_EQ(Number(service->Status(), "usedRequests"), 3u);
}
}

namespace Alles::Bridge
{
TEST_F(AllesPlanningBridgeTest, PlanningAndConversationShareTheQueueAdmissionLimit)
{
    service->SetQueueLimit(2);
    ASSERT_TRUE(service->QueuePlanning("plan", {}, now));
    ASSERT_TRUE(service->QueueConversation("chat", {}, now));
    EXPECT_EQ(service->PendingJobs(), 2u);
    EXPECT_FALSE(service->QueuePlanning("extra", {}, now));
    service->SetQueueLimit(1);
    EXPECT_EQ(service->PendingJobs(), 2u); // Existing work drains; a lower limit does not discard it.
    EXPECT_FALSE(service->QueueConversation("another", {}, now));
    service->CancelPlanning("plan");
    service->Update(now, now);
    EXPECT_EQ(service->PendingJobs(), 1u);
    EXPECT_FALSE(service->QueuePlanning("still-full", {}, now));
    service->CancelConversation("chat");
    service->Update(now, now);
    EXPECT_EQ(service->PendingJobs(), 0u);
    EXPECT_TRUE(service->QueuePlanning("next", {}, now));
}
}
