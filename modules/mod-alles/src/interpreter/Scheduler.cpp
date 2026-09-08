/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "Scheduler.h"
#include <algorithm>
#include <stdexcept>

namespace Alles::Interpreter
{
namespace
{
uint64_t Priority(ScheduledJob const& job, uint64_t now)
{
    // Aging overtakes the entire priority range after 15 seconds; actors rotate before priorities matter.
    uint64_t const base = job.purpose == JobPurpose::Conversation ? 3 :
        job.purpose == JobPurpose::Interview ? 0 : 1;
    return base + (now >= job.admittedMs ? (now - job.admittedMs) / 5000 : 0);
}
}

bool ValidPolicy(SchedulingPolicy const& p)
{
    return p.revision && p.concurrentJobs && p.concurrentJobs <= 32 && p.waitingPerActor &&
        p.waitingPerActor <= 64 && p.waitingGlobal >= p.waitingPerActor && p.waitingGlobal <= 256 &&
        p.bytesPerActor >= 1024 && p.bytesPerActor <= 1024 * 1024 &&
        p.bytesGlobal >= p.bytesPerActor && p.bytesGlobal <= 16 * 1024 * 1024 &&
        p.maxWaitMs >= 1000 && p.maxWaitMs <= 20000;
}

Scheduler::Scheduler(SchedulingPolicy value) : policy(value)
{
    if (!ValidPolicy(policy))
        throw std::invalid_argument("invalid scheduler policy");
}

std::size_t Scheduler::Active() const
{
    return std::count_if(jobs.begin(), jobs.end(), [](auto const& item)
        { return item.second.state != JobState::Queued; });
}

bool Scheduler::Fits(ScheduledJob const& job) const
{
    std::size_t count = 0, ownCount = 0, bytes = 0, ownBytes = 0;
    for (auto const& [id, item] : jobs)
        if (item.state == JobState::Queued)
        {
            ++count;
            bytes += item.bytes;
            if (item.actor == job.actor)
            {
                ++ownCount;
                ownBytes += item.bytes;
            }
        }
    return count < policy.waitingGlobal && ownCount < policy.waitingPerActor &&
        bytes + job.bytes <= policy.bytesGlobal && ownBytes + job.bytes <= policy.bytesPerActor;
}

std::string Scheduler::Admit(ScheduledJob job)
{
    if (job.id.empty() || job.id.size() > 64 || !IsValidActor(job.actor) || !job.bytes ||
        job.expiresMs <= job.admittedMs || jobs.contains(job.id))
        return "invalid_job";
    job.expiresMs = std::min(job.expiresMs, job.admittedMs + policy.maxWaitMs);
    // Replacement is only allowed within the same actor, generation, purpose and explicit decision key.
    auto old = std::find_if(jobs.begin(), jobs.end(), [&](auto const& pair)
    {
        auto const& item = pair.second;
        return !job.replacementKey.empty() && item.state == JobState::Queued && item.actor == job.actor &&
            item.generation == job.generation && item.purpose == job.purpose &&
            item.replacementKey == job.replacementKey;
    });
    if (old != jobs.end())
    {
        job.admittedMs = old->second.admittedMs;
        job.expiresMs = std::min(job.expiresMs, old->second.expiresMs);
        job.sequence = old->second.sequence;
        // Keep the old request if the replacement cannot fit; no unrelated work is displaced at admission.
        auto previous = old->second;
        jobs.erase(old);
        if (!Fits(job))
        {
            jobs.emplace(previous.id, std::move(previous));
            ++rejected;
            return "queue_capacity";
        }
        outcomes.push_back({previous.id, "superseded", previous.purpose});
    }
    else if (!Fits(job))
    {
        ++rejected;
        return "queue_capacity";
    }
    if (!job.sequence)
        job.sequence = ++sequence;
    if (std::find(actors.begin(), actors.end(), job.actor) == actors.end())
        actors.push_back(job.actor);
    jobs.emplace(job.id, std::move(job));
    return {};
}

std::optional<ScheduledJob> Scheduler::Select(uint64_t now, std::vector<JobPurpose> const& supported)
{
    Expire(now);
    if (Active() >= policy.concurrentJobs)
        return std::nullopt;
    for (auto actor : actors)
    {
        bool const busy = std::any_of(jobs.begin(), jobs.end(), [&](auto const& pair)
            { return pair.second.actor == actor && pair.second.state != JobState::Queued; });
        if (busy)
            continue;
        ScheduledJob const* best = nullptr;
        for (auto const& [id, item] : jobs)
        {
            if (item.actor != actor || item.state != JobState::Queued ||
                std::find(supported.begin(), supported.end(), item.purpose) == supported.end())
                continue;
            bool const preceded = !item.causalKey.empty() && std::any_of(jobs.begin(), jobs.end(),
                [&](auto const& pair)
                {
                    auto const& other = pair.second;
                    return other.actor == actor && other.causalKey == item.causalKey &&
                        other.sequence < item.sequence;
                });
            if (preceded)
                continue;
            if (!best || Priority(item, now) > Priority(*best, now) ||
                (Priority(item, now) == Priority(*best, now) && item.sequence < best->sequence))
                best = &item;
        }
        if (best)
            return *best;
    }
    return std::nullopt;
}

bool Scheduler::Reserve(std::string const& id, uint64_t now)
{
    auto found = jobs.find(id);
    if (found == jobs.end() || found->second.state != JobState::Queued || now >= found->second.expiresMs ||
        Active() >= policy.concurrentJobs)
        return false;
    auto& job = found->second;
    if (std::any_of(jobs.begin(), jobs.end(), [&](auto const& pair)
        { return pair.second.actor == job.actor && pair.second.state != JobState::Queued; }))
        return false;
    job.state = JobState::Reserved;
    job.dispatchedMs = now;
    std::erase(actors, job.actor);
    actors.push_back(job.actor);
    return true;
}

void Scheduler::State(std::string const& id, JobState state)
{
    if (auto found = jobs.find(id); found != jobs.end() && state != JobState::Queued)
        found->second.state = state;
}

void Scheduler::RemoveActor(ActorKey actor)
{
    if (std::none_of(jobs.begin(), jobs.end(), [&](auto const& pair) { return pair.second.actor == actor; }))
        std::erase(actors, actor);
}

void Scheduler::Finish(std::string const& id, std::string reason)
{
    auto found = jobs.find(id);
    if (found == jobs.end())
        return;
    auto const actor = found->second.actor;
    outcomes.push_back({id, std::move(reason), found->second.purpose});
    jobs.erase(found);
    RemoveActor(actor);
}

void Scheduler::Expire(uint64_t now)
{
    std::vector<std::string> expired;
    for (auto const& [id, job] : jobs)
        if (job.state == JobState::Queued && now >= job.expiresMs)
            expired.push_back(id);
    for (auto const& id : expired)
        Finish(id, "waiting_expired");
}

void Scheduler::Apply(SchedulingPolicy value)
{
    if (!ValidPolicy(value))
        throw std::invalid_argument("invalid scheduler policy");
    policy = value;
    // On shrink, keep oldest work; newest lowest-priority waiting work is evicted first.
    std::vector<ScheduledJob> waiting;
    for (auto const& [id, job] : jobs)
        if (job.state == JobState::Queued)
            waiting.push_back(job);
    std::sort(waiting.begin(), waiting.end(), [](auto const& a, auto const& b)
    {
        auto const pa = Priority(a, a.admittedMs), pb = Priority(b, b.admittedMs);
        return pa != pb ? pa > pb : a.sequence < b.sequence;
    });
    for (auto const& job : waiting)
        jobs.erase(job.id);
    for (auto job : waiting)
    {
        if (Fits(job))
        {
            job.expiresMs = std::min(job.expiresMs, job.admittedMs + policy.maxWaitMs);
            jobs.emplace(job.id, std::move(job));
        }
        else
        {
            outcomes.push_back({job.id, "policy_capacity", job.purpose});
            RemoveActor(job.actor);
        }
    }
    for (auto const& [id, job] : jobs)
        if (std::find(actors.begin(), actors.end(), job.actor) == actors.end())
            actors.push_back(job.actor);
}

std::vector<QueueOutcome> Scheduler::TakeOutcomes()
{
    std::vector<QueueOutcome> result;
    result.swap(outcomes);
    return result;
}
}
