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
    void Update(uint64_t gameMs, uint64_t realMs);
    Transport& IO()
    {
        return transport;
    }
    boost::json::object Status() const;
    bool QueueConversation(std::string id, boost::json::object context, uint64_t realMs);
    void CancelConversation(std::string const& id);
    std::vector<ConversationResult> TakeConversations();
    bool QueuePlanning(std::string id, boost::json::object context, uint64_t realMs);
    void CancelPlanning(std::string const& id);
    std::vector<PlanningResult> TakePlanning();

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
    };
    enum class Purpose : uint8_t { Conversation, Planning };
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
    bool QueueWorkerJob(std::string id, boost::json::object context, uint64_t realMs, Purpose purpose);
    bool Available(uint64_t gameMs) const;
    void Charge(uint64_t gameMs);
    bool Busy(uint64_t realMs) const;
    boost::json::object Handle(uint64_t connection, boost::json::object const& request, uint64_t gameMs,
                               uint64_t realMs);
    Interpreter::PilotCoordinator& coordinator;
    Settings settings;
    Transport transport;
    std::map<std::string, Interpreter::JobSnapshot> claims;
    std::map<std::string, Attempt> attempts;
    std::set<uint64_t> workers;
    std::set<uint64_t> planningWorkers;
    std::deque<WorkerJob> workerJobs;
    std::vector<ConversationResult> conversationResults;
    std::vector<PlanningResult> planningResults;
    std::deque<uint64_t> reservations;
    uint64_t conversationCompleted = 0;
    uint64_t conversationFailed = 0;
    uint64_t planningCompleted = 0;
    uint64_t planningFailed = 0;
    uint64_t epochMs = 0;
    uint32_t charged = 0;
    uint64_t cooldown = 0;
    uint64_t lastWorkerMs = 0;
    uint64_t nowMs = 0;
    bool fault = false;
    uint8_t nextPurpose = 0; // Round robin: memory, objective planning, conversation.
};
} // namespace Alles::Bridge
#endif
