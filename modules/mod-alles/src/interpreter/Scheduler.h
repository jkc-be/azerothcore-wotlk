/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_SCHEDULER_H
#define MOD_ALLES_SCHEDULER_H

#include "domain/Memory.h"
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Alles::Interpreter
{
enum class JobPurpose : uint8_t
{
    Memory,
    Planning,
    Conversation,
    Interview
};
enum class JobState : uint8_t
{
    Queued,
    Reserved,
    Running,
    ResultWaiting
};

struct SchedulingPolicy
{
    uint64_t revision = 1;
    std::size_t concurrentJobs = 1;
    std::size_t waitingPerActor = 2;
    std::size_t waitingGlobal = 10;
    std::size_t bytesPerActor = 49152;
    std::size_t bytesGlobal = 245760;
    uint64_t maxWaitMs = 20000;
};

bool ValidPolicy(SchedulingPolicy const& policy);

struct ScheduledJob
{
    std::string id;
    ActorKey actor;
    uint64_t generation = 0;
    JobPurpose purpose = JobPurpose::Memory;
    std::string replacementKey;
    std::string causalKey;
    uint64_t admittedMs = 0;
    uint64_t expiresMs = 0;
    std::size_t bytes = 0;
    JobState state = JobState::Queued;
    uint64_t sequence = 0;
    uint64_t dispatchedMs = 0;
};

struct QueueOutcome
{
    std::string id;
    std::string reason;
    JobPurpose purpose = JobPurpose::Memory;
};

// World-thread descriptor index only: evidence and result ownership stay with each purpose's runtime.
// Active jobs retain their actor and global slot until physical completion or a bounded execution fence.
class Scheduler
{
public:
    explicit Scheduler(SchedulingPolicy policy = {});
    std::string Admit(ScheduledJob job);
    std::optional<ScheduledJob> Select(uint64_t realMs, std::vector<JobPurpose> const& supported);
    bool Reserve(std::string const& id, uint64_t realMs);
    void State(std::string const& id, JobState state);
    void Finish(std::string const& id, std::string reason);
    void Expire(uint64_t realMs);
    void Apply(SchedulingPolicy policy);
    std::vector<QueueOutcome> TakeOutcomes();
    std::map<std::string, ScheduledJob> const& Jobs() const
    {
        return jobs;
    }
    SchedulingPolicy const& Policy() const
    {
        return policy;
    }
    bool Contains(std::string const& id) const
    {
        return jobs.contains(id);
    }
    std::size_t Active() const;
    uint64_t Rejected() const
    {
        return rejected;
    }

private:
    bool Fits(ScheduledJob const& job) const;
    void RemoveActor(ActorKey actor);
    SchedulingPolicy policy;
    std::map<std::string, ScheduledJob> jobs;
    std::deque<ActorKey> actors;
    std::vector<QueueOutcome> outcomes;
    uint64_t sequence = 0;
    uint64_t rejected = 0;
};
}
#endif
