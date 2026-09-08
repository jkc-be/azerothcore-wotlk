/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_BRIDGE_SERVICE_H
#define MOD_ALLES_BRIDGE_SERVICE_H

#include "AllesTransport.h"
#include "Wire.h"
#include "interpreter/Scheduler.h"
#include <functional>
#include <array>
#include <set>
#include <deque>

namespace Alles::Bridge
{
struct Settings
{
    uint16_t port = 8779;
    std::string token;
    std::string profile;
    std::string model;
    std::string ledger;
    uint32_t maxRequests = 100;
    uint32_t requestsPerMinute = 0; // Nonzero enables continuous operation instead of a lifetime trial cap.
    std::string budgetMode = "trial";
    Interpreter::SchedulingPolicy policy;
    std::string policyFile;
    std::string controlToken;
    std::string run;
    std::size_t concurrentCalls = 1;
};

struct ConversationResult
{
    std::string id;
    std::string status;
    boost::json::object response;
};

using PlanningResult = ConversationResult;

class Service
{
public:
    Service(Interpreter::PilotCoordinator& coordinator, Settings settings);
    void Update(uint64_t gameMs, uint64_t realMs, bool paused = false, uint64_t realEpochMs = 0);
    Transport& IO()
    {
        return transport;
    }
    boost::json::object Status() const;
    bool QueueConversation(std::string id, boost::json::object context, uint64_t realMs,
        ActorKey actor, uint64_t generation = 0, std::string causalKey = {});
    void CancelConversation(std::string const& id);
    std::vector<ConversationResult> TakeConversations();
    bool QueuePlanning(std::string id, boost::json::object context, uint64_t realMs,
        ActorKey actor, uint64_t generation = 0, std::string replacementKey = {});
    void CancelPlanning(std::string const& id);
    std::vector<PlanningResult> TakePlanning();
    using InterviewContext = std::function<std::optional<boost::json::object>(ActorKey)>;
    void SetInterviewContext(InterviewContext callback)
    {
        interviewContext = std::move(callback);
    }
    void CancelActor(ActorKey actor);

private:
    struct Attempt
    {
        Interpreter::JobSnapshot job;
        std::string id;
        std::string requestId;
        std::string state = "pending";
        std::string outcome;
        uint64_t expires = 0;
        bool complete = false;
        bool reported = false;
    };
    using Purpose = Interpreter::JobPurpose;
    struct WorkerJob
    {
        std::string id;
        boost::json::object context;
        uint64_t admitted = 0;
        uint64_t expires = 0;
        std::string worker;
        std::string permit;
        std::string state = "queued";
        bool cancelled = false;
        Purpose purpose = Purpose::Conversation;
    };
    bool QueueWorkerJob(std::string id, boost::json::object context, uint64_t realMs, Purpose purpose,
        ActorKey actor, uint64_t generation, std::string replacementKey = {}, std::string causalKey = {});
    void Reconcile(uint64_t gameMs, uint64_t realMs);
    boost::json::object PolicyStatus() const;
    void ApplyPolicy(boost::json::object const& policy);
    boost::json::object Control(std::string const& op, boost::json::object const& args, uint64_t realMs);

    bool Available(uint64_t realEpochMs) const;
    void Charge(uint64_t realEpochMs);
    boost::json::object Handle(uint64_t connection, boost::json::object const& request, uint64_t gameMs,
                               uint64_t realMs);
    Interpreter::PilotCoordinator& coordinator;
    Settings settings;
    Transport transport;
    std::map<std::string, Interpreter::JobSnapshot> claims;
    std::map<std::string, Attempt> attempts;
    std::set<uint64_t> workers;
    std::set<uint64_t> planningWorkers;
    std::set<uint64_t> interviewWorkers;
    std::set<std::string> heldFallbacks;
    std::deque<WorkerJob> workerJobs;
    Interpreter::Scheduler scheduler;
    InterviewContext interviewContext;
    struct InterviewReceipt
    {
        uint64_t expires = 0;
        boost::json::object result;
    };
    std::map<std::string, InterviewReceipt> interviews;
    boost::json::object pendingPolicy;
    std::string policyCommand;
    std::string policyRequest;
    std::string policyError;
    std::map<std::string, uint64_t> outcomes;
    uint64_t totalWaitMs = 0;
    std::array<uint64_t, 6> waitHistogram{};
    std::array<uint64_t, 6> serviceHistogram{};
    uint64_t dispatchedJobs = 0;
    uint64_t backendCalls = 0;
    uint64_t unknownUsageJobs = 0;
    uint64_t promptTokens = 0;
    uint64_t completionTokens = 0;
    uint64_t serviceMs = 0;
    std::map<uint64_t, std::size_t> workerCapacity;

    std::vector<ConversationResult> conversationResults;
    std::vector<PlanningResult> planningResults;
    ReservationHistory reservations;
    uint64_t conversationCompleted = 0;
    uint64_t conversationFailed = 0;
    uint64_t planningCompleted = 0;
    uint64_t planningFailed = 0;
    uint64_t epochMs = 0;
    uint64_t charged = 0;
    uint64_t cooldown = 0;
    bool recoveringReservations = false;
    uint64_t lastWorkerMs = 0;
    uint64_t nowMs = 0;
    bool fault = false;
    bool paused = false;
    std::vector<Frame> heldResults;
};
} // namespace Alles::Bridge
#endif
