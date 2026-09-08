/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Service.h"
#include "PlanningWire.h"
#include "perception/SpeechRoute.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Alles::Bridge
{
namespace
{
constexpr std::array<uint64_t, 5> LatencyBounds{1000, 5000, 15000, 30000, 60000};
void ObserveLatency(std::array<uint64_t, 6>& histogram, uint64_t ms)
{
    auto bucket = std::upper_bound(LatencyBounds.begin(), LatencyBounds.end(), ms) - LatencyBounds.begin();
    ++histogram[bucket];
}
boost::json::array DescribeLatency(std::array<uint64_t, 6> const& histogram)
{
    boost::json::array result;
    for (std::size_t index = 0; index < histogram.size(); ++index)
        result.emplace_back(boost::json::object{{"belowMs", index < LatencyBounds.size() ?
            boost::json::value(LatencyBounds[index]) : boost::json::value(nullptr)}, {"count", histogram[index]}});
    return result;
}
}

Service::Service(Interpreter::PilotCoordinator& value, Settings config)
    : coordinator(value), settings(std::move(config)),
      transport(settings.port, settings.ledger, settings.profile, 0), scheduler(settings.policy),
      charged(transport.Charged())
{
    if (!settings.policyFile.empty() && std::filesystem::exists(settings.policyFile))
    {
        if (std::filesystem::file_size(settings.policyFile) > 4096)
            throw std::invalid_argument("policy override too large");
        std::ifstream file(settings.policyFile);
        std::string data((std::istreambuf_iterator<char>(file)), {});
        ApplyPolicy(Parse(data).as_object());
    }
    coordinator.EnableExternal();
    reservations = transport.RecentReservations();
    recoveringReservations = !reservations.empty();
}

boost::json::object Service::PolicyStatus() const
{
    auto const& p = scheduler.Policy();
    std::size_t negotiated = 0;
    for (auto const& [connection, capacity] : workerCapacity)
        negotiated += capacity;
    return {{"schema", 1}, {"revision", p.revision}, {"concurrentJobs", p.concurrentJobs},
        {"waitingPerActor", p.waitingPerActor}, {"waitingGlobal", p.waitingGlobal},
        {"bytesPerActor", p.bytesPerActor}, {"bytesGlobal", p.bytesGlobal}, {"maxWaitMs", p.maxWaitMs},
        {"mode", settings.budgetMode == "unlimited" ? "unlimited" :
            settings.requestsPerMinute ? "limited" : "trial"},
        {"modelRpm", settings.requestsPerMinute}, {"concurrentCalls", settings.concurrentCalls},
        {"trialMaxCalls", settings.maxRequests}, {"maxCallsPerJob", 1},
        {"effectiveConcurrentJobs", std::min({p.concurrentJobs, settings.concurrentCalls, negotiated})},
        {"pending", !pendingPolicy.empty()}, {"requested", pendingPolicy},
        {"command", policyCommand}, {"error", policyError},
        {"run", settings.run}, {"controls", !settings.controlToken.empty() && !settings.policyFile.empty()}};
}

namespace
{
Interpreter::SchedulingPolicy DecodePolicy(boost::json::object const& value)
{
    Fields(value, {"schema", "revision", "concurrentJobs", "waitingPerActor", "waitingGlobal", "bytesPerActor",
        "bytesGlobal", "maxWaitMs", "mode", "modelRpm", "concurrentCalls", "trialMaxCalls"});
    if (Number(value, "schema") != 1)
        throw std::invalid_argument("unsupported policy schema");
    Interpreter::SchedulingPolicy p;
    p.revision = Number(value, "revision");
    p.concurrentJobs = Number(value, "concurrentJobs");
    p.waitingPerActor = Number(value, "waitingPerActor");
    p.waitingGlobal = Number(value, "waitingGlobal");
    p.bytesPerActor = Number(value, "bytesPerActor");
    p.bytesGlobal = Number(value, "bytesGlobal");
    p.maxWaitMs = Number(value, "maxWaitMs");
    auto const mode = String(value, "mode", 16);
    auto const rpm = Number(value, "modelRpm");
    auto const calls = Number(value, "concurrentCalls");
    auto const trial = Number(value, "trialMaxCalls");
    if (!Interpreter::ValidPolicy(p) || !calls || calls > 32 || rpm > 100000 || trial > UINT32_MAX ||
        (mode != "limited" && mode != "unlimited" && mode != "trial") ||
        (mode == "limited" && !rpm) || (mode == "trial" && !trial))
        throw std::invalid_argument("invalid policy bounds");
    return p;
}
}

void Service::ApplyPolicy(boost::json::object const& value)
{
    auto const policy = DecodePolicy(value);
    scheduler.Apply(policy);
    settings.budgetMode = String(value, "mode");
    settings.requestsPerMinute = settings.budgetMode == "limited" ? uint32_t(Number(value, "modelRpm")) : 0;
    settings.maxRequests = uint32_t(Number(value, "trialMaxCalls"));
    settings.concurrentCalls = Number(value, "concurrentCalls");
    settings.policy = policy;
}

boost::json::object Service::Control(std::string const& op, boost::json::object const& args, uint64_t realMs)
{
    if (op == "interpreter_status")
    {
        Fields(args, {});
        return Status();
    }
    if (op == "interpreter_policy")
    {
        Fields(args, {"run", "command", "expectedRevision", "policy"});
        auto const command = String(args, "command", 64);
        if (String(args, "run") != settings.run)
            return {{"error", "Run changed; refresh before applying policy"}};
        if (command.empty() || settings.policyFile.empty())
            return {{"error", "Managed policy storage is unavailable"}};
        if (command == policyCommand)
            return policyRequest == boost::json::serialize(args) ? PolicyStatus() :
                boost::json::object{{"error", "Command identifier was reused with different settings"}};
        if (!pendingPolicy.empty())
            return {{"error", "Another policy is being persisted"}};
        if (Number(args, "expectedRevision") != scheduler.Policy().revision)
            return {{"error", "Stale policy revision; refresh before applying"}};
        auto value = args.at("policy").as_object();
        if (scheduler.Policy().revision == UINT64_MAX)
            return {{"error", "Policy revision exhausted"}};
        value["revision"] = scheduler.Policy().revision + 1;
        try
        {
            DecodePolicy(value);
        }
        catch (std::exception const&)
        {
            return {{"error", "Invalid policy schema, mode, queue, rate or concurrency bounds"}};
        }
        if (!transport.PersistPolicy(settings.policyFile, command, boost::json::serialize(value)))
            return {{"error", "Policy mailbox is full"}};
        policyCommand = command;
        policyRequest = boost::json::serialize(args);
        policyError.clear();
        pendingPolicy = std::move(value);
        return PolicyStatus();
    }
    if (op == "interview_status")
    {
        Fields(args, {"run", "jobToken"});
        if (String(args, "run") != settings.run)
            return {{"status", "stale"}};
        auto found = interviews.find(String(args, "jobToken", 64));
        return found == interviews.end() ? boost::json::object{{"status", "unknown"}} : found->second.result;
    }
    Fields(args, {"run", "jobToken", "owner", "context"});
    if (String(args, "run") != settings.run)
        return {{"error", "Run changed"}};
    auto const id = String(args, "jobToken", 64);
    if (!id.starts_with("interview-") || interviews.size() >= 16)
        return {{"error", "Interview capacity exhausted or invalid identifier"}};
    if (auto found = interviews.find(id); found != interviews.end())
        return found->second.result;
    auto const& owner = args.at("owner").as_object();
    Fields(owner, {"kind", "id"});
    if (Number(owner, "kind") > 1)
        throw std::invalid_argument("invalid actor kind");
    ActorKey actor{ActorKind(Number(owner, "kind")), Number(owner, "id")};
    auto personal = interviewContext ? interviewContext(actor) : std::nullopt;
    if (!personal)
        return {{"error", "Character is not a configured live owner"}};
    auto context = args.at("context").as_object();
    Fields(context, {"character", "place", "asked", "memories", "history", "message", "retrieval"});
    context["personalState"] = std::move(*personal);
    if (!QueueWorkerJob(id, std::move(context), realMs, Purpose::Interview, actor, 0))
        return {{"error", "Interview queue is full"}};
    interviews.emplace(id, InterviewReceipt{realMs + 60000, {{"status", "queued"}}});
    return {{"status", "queued"}};
}

void Service::Reconcile(uint64_t gameMs, uint64_t realMs)
{
    scheduler.Expire(realMs);
    std::vector<std::string> finished;
    for (auto const& [id, job] : scheduler.Jobs())
    {
        if (job.purpose != Purpose::Memory)
            continue;
        auto current = coordinator.Inspect(job.actor);
        if (job.state == Interpreter::JobState::Queued)
        {
            if (!current || current->jobToken != id)
                finished.push_back(id);
        }
        else
        {
            auto attempt = attempts.find(id);
            auto claim = claims.find(id);
            if ((attempt != attempts.end() && (realMs >= attempt->second.expires ||
                (attempt->second.complete && !attempt->second.outcome.empty()))) ||
                (attempt == attempts.end() && (claim == claims.end() ||
                    realMs >= claim->second.leaseExpiresRealTimeMs)))
                finished.push_back(id);
        }
    }
    for (auto const& id : finished)
        scheduler.Finish(id, "memory_finished_or_expired");
    for (auto const& outcome : scheduler.TakeOutcomes())
    {
        ++outcomes[outcome.reason];
        if (outcome.purpose == Purpose::Memory)
        {
            if (paused)
                heldFallbacks.insert(outcome.id); // At most the coordinator's 64 pre-pause memory jobs.
            else
                coordinator.RejectQueued(outcome.id, gameMs, realMs);
        }
        auto found = std::find_if(workerJobs.begin(), workerJobs.end(),
            [&](auto const& item) { return item.id == outcome.id; });
        if (found != workerJobs.end() && found->state == "queued")
        {
            if (found->purpose == Purpose::Interview)
            {
                if (auto receipt = interviews.find(found->id); receipt != interviews.end())
                    receipt->second.result = {{"status", outcome.reason}};
            }
            else
            {
                auto& results = found->purpose == Purpose::Planning ? planningResults : conversationResults;
                if (results.size() < 64)
                    results.push_back({found->id, outcome.reason, {}});
            }
            workerJobs.erase(found);
        }
    }
    if (paused)
        return;
    for (auto const& id : heldFallbacks)
        coordinator.RejectQueued(id, gameMs, realMs);
    heldFallbacks.clear();
    for (auto const& job : coordinator.Ready(gameMs, realMs))
    {
        if (scheduler.Contains(job.jobToken))
            continue;
        auto error = scheduler.Admit({job.jobToken, job.owner, job.actorGeneration, Purpose::Memory, {}, "memory",
            job.admittedRealTimeMs, job.admittedRealTimeMs + 20000, job.contextBytesBound});
        if (!error.empty())
        {
            ++outcomes[error];
            coordinator.RejectQueued(job.jobToken, gameMs, realMs);
        }
    }
}

bool Service::Available(uint64_t realEpochMs) const
{
    if (charged == UINT64_MAX)
        return false;
    if (settings.budgetMode == "unlimited")
        return true;
    if (!settings.requestsPerMinute)
        return charged < settings.maxRequests;
    return RecentCount(reservations, realEpochMs) < settings.requestsPerMinute;
}

void Service::Charge(uint64_t realEpochMs)
{
    try
    {
        if (charged == UINT64_MAX)
            throw std::overflow_error("reservation count exhausted");
        AddReservation(reservations, realEpochMs);
        ++charged;
    }
    catch (...)
    {
        fault = true;
        throw;
    }
}

bool Service::QueueWorkerJob(std::string id, boost::json::object context, uint64_t realMs, Purpose purpose,
    ActorKey actor, uint64_t generation, std::string replacementKey, std::string causalKey)
{
    auto const& results = purpose == Purpose::Planning ? planningResults : conversationResults;
    auto const bytes = boost::json::serialize(context).size();
    if (results.size() + workerJobs.size() >= 64 || bytes > 12288)
        return false;
    Interpreter::ScheduledJob descriptor{id, actor, generation, purpose, std::move(replacementKey),
        std::move(causalKey), realMs, realMs + scheduler.Policy().maxWaitMs, bytes};
    auto const error = scheduler.Admit(std::move(descriptor));
    if (!error.empty())
    {
        ++outcomes[error];
        return false;
    }
    WorkerJob job{std::move(id), std::move(context), realMs, realMs + scheduler.Policy().maxWaitMs + 25000};
    job.purpose = purpose;
    workerJobs.push_back(std::move(job));
    return true;
}

bool Service::QueueConversation(std::string id, boost::json::object context, uint64_t realMs,
    ActorKey actor, uint64_t generation, std::string causalKey)
{
    return QueueWorkerJob(std::move(id), std::move(context), realMs, Purpose::Conversation,
        actor, generation, {}, std::move(causalKey));
}

bool Service::QueuePlanning(std::string id, boost::json::object context, uint64_t realMs,
    ActorKey actor, uint64_t generation, std::string replacementKey)
{
    return QueueWorkerJob(std::move(id), std::move(context), realMs, Purpose::Planning,
        actor, generation, std::move(replacementKey));
}

void Service::CancelActor(ActorKey actor)
{
    for (auto& item : workerJobs)
        if (auto job = scheduler.Jobs().find(item.id); job != scheduler.Jobs().end() && job->second.actor == actor)
            item.cancelled = true;
}

void Service::CancelConversation(std::string const& id)
{
    for (auto& item : workerJobs)
        if (item.id == id && item.purpose == Purpose::Conversation)
            item.cancelled = true;
}

void Service::CancelPlanning(std::string const& id)
{
    for (auto& item : workerJobs)
        if (item.id == id && item.purpose == Purpose::Planning)
            item.cancelled = true;
}

std::vector<ConversationResult> Service::TakeConversations()
{
    std::vector<ConversationResult> result;
    result.swap(conversationResults);
    for (auto const& item : result)
        if (auto job = scheduler.Jobs().find(item.id); job != scheduler.Jobs().end() &&
            job->second.state == Interpreter::JobState::ResultWaiting)
            scheduler.Finish(item.id, "result_handed_to_runtime");
    return result;
}

std::vector<PlanningResult> Service::TakePlanning()
{
    std::vector<PlanningResult> result;
    result.swap(planningResults);
    for (auto const& item : result)
        if (auto job = scheduler.Jobs().find(item.id); job != scheduler.Jobs().end() &&
            job->second.state == Interpreter::JobState::ResultWaiting)
            scheduler.Finish(item.id, "result_handed_to_runtime");
    return result;
}

boost::json::object Service::Status() const
{
    boost::json::object reasons;
    for (auto const& [reason, count] : outcomes)
        reasons[reason] = count;
    boost::json::array actors;
    std::map<ActorKey, boost::json::object> queues;
    std::size_t waiting = 0, bytes = 0;
    uint64_t oldest = 0;
    for (auto const& [id, job] : scheduler.Jobs())
    {
        auto& row = queues[job.actor];
        if (row.empty())
            row = {{"owner", std::string(job.actor.kind == ActorKind::Player ? "player:" : "creature:") +
                std::to_string(job.actor.id)}, {"waiting", 0}, {"bytes", 0}, {"active", 0}, {"oldestMs", 0}};
        if (job.state == Interpreter::JobState::Queued)
        {
            ++waiting;
            bytes += job.bytes;
            uint64_t const age = nowMs >= job.admittedMs ? nowMs - job.admittedMs : 0;
            oldest = std::max(oldest, age);
            row["waiting"] = Number(row, "waiting") + 1;
            row["bytes"] = Number(row, "bytes") + job.bytes;
            row["oldestMs"] = std::max(Number(row, "oldestMs"), age);
        }
        else
            row["active"] = Number(row, "active") + 1;
    }
    for (auto& [owner, row] : queues)
        actors.emplace_back(std::move(row));
    boost::json::object result{{"mode", "provider"},
            {"policy", PolicyStatus()}, {"waitingJobs", waiting}, {"waitingBytes", bytes},
            {"oldestWaitingMs", oldest}, {"activeJobs", scheduler.Active()}, {"actors", std::move(actors)},
            {"outcomes", std::move(reasons)}, {"dispatchedJobs", dispatchedJobs},
            {"meanWaitMs", dispatchedJobs ? totalWaitMs / dispatchedJobs : 0},
            {"waitLatency", DescribeLatency(waitHistogram)}, {"serviceLatency", DescribeLatency(serviceHistogram)},
            {"backendCalls", backendCalls}, {"unknownUsageJobs", unknownUsageJobs},
            {"promptTokens", promptTokens}, {"completionTokens", completionTokens},
            {"meanServiceMs", backendCalls ? serviceMs / backendCalls : 0},
            {"connected", !workers.empty() && lastWorkerMs && nowMs - lastWorkerMs < 30000},
            {"model", settings.model},
            {"profile", settings.profile},
            {"usedRequests", charged},
            {"maxRequests", settings.maxRequests},
            {"ledgerFault", fault},
            {"modelMemories", coordinator.Stats().modelMemories},
            {"invalidResults", coordinator.Stats().invalidResults},
            {"fallbackMemories", coordinator.Stats().fallbackMemories},
            {"gameDeadlineExpiries", coordinator.Stats().gameDeadlineExpiries},
            {"realDeadlineExpiries", coordinator.Stats().realDeadlineExpiries},
            {"leaseExpiries", coordinator.Stats().leaseExpiries},
            {"budgetMode", settings.budgetMode == "unlimited" ? "unlimited" :
                settings.requestsPerMinute ? "rolling" : "trial"},
            {"requestsPerMinute", settings.requestsPerMinute},
            {"remainingRequests",
             settings.requestsPerMinute
                 ? settings.requestsPerMinute -
                    std::min<std::size_t>(settings.requestsPerMinute, RecentCount(reservations, epochMs))
                 : settings.maxRequests - std::min<uint64_t>(settings.maxRequests, charged)},
            {"conversationQueued", std::count_if(workerJobs.begin(), workerJobs.end(),
                [](auto const& item) { return item.purpose == Purpose::Conversation; })},
            {"planningQueued", std::count_if(workerJobs.begin(), workerJobs.end(),
                [](auto const& item) { return item.purpose == Purpose::Planning; })},
            {"planningCompleted", planningCompleted},
            {"planningFailed", planningFailed},
            {"conversationCompleted", conversationCompleted},
            {"conversationFailed", conversationFailed}};
    if (settings.budgetMode == "unlimited")
        result["remainingRequests"] = nullptr;
    return result;
}

void Service::Update(uint64_t gameMs, uint64_t realMs, bool hold, uint64_t realEpochMs)
{
    nowMs = realMs;
    paused = hold;
    epochMs = realEpochMs ? realEpochMs : std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    PruneReservations(reservations, epochMs);
    if (recoveringReservations)
    {
        if (!reservations.empty())
            cooldown = std::max(cooldown, realMs + 45000);
        recoveringReservations = false;
    }
    Reconcile(gameMs, realMs);
    std::erase_if(workerJobs,
                  [&](WorkerJob const& item)
                  {
                      if (realMs < item.expires && !(item.cancelled && item.state == "queued"))
                          return false;
                      auto& results = item.purpose == Purpose::Planning ? planningResults : conversationResults;
                      if (item.purpose == Purpose::Interview)
                      {
                          if (auto receipt = interviews.find(item.id); receipt != interviews.end())
                              receipt->second.result = {{"status", item.cancelled ? "cancelled" : "timeout"}};
                      }
                      else if (!item.cancelled && results.size() < 64)
                          results.push_back({item.id, "timeout", {}});
                      if (item.state != "failed")
                          ++(item.purpose == Purpose::Planning ? planningFailed : conversationFailed);
                      scheduler.Finish(item.id, item.cancelled ? "cancelled" : "execution_timeout");
                      return true;
                  });
    auto frames = transport.Poll();
    if (!paused && !heldResults.empty())
    {
        frames.insert(frames.begin(), std::make_move_iterator(heldResults.begin()),
            std::make_move_iterator(heldResults.end()));
        heldResults.clear();
    }
    for (auto const& frame : frames)
    {
        boost::json::value id;
        try
        {
            auto request = Parse(frame.text).as_object();
            if (!frame.connection)
            {
                if (request.contains("disconnected"))
                {
                    auto const connection = Number(request, "disconnected");
                    workers.erase(connection);
                    planningWorkers.erase(connection);
                    interviewWorkers.erase(connection);
                    workerCapacity.erase(connection);
                    continue; // Physical jobs keep their bounded execution fences after disconnect.
                }
                if (request.contains("command"))
                {
                    if (String(request, "command") == policyCommand && !pendingPolicy.empty())
                    {
                        if (request.at("ok").as_bool())
                            ApplyPolicy(pendingPolicy);
                        else
                            policyError = "Policy persistence failed; active settings retained";
                        pendingPolicy.clear();
                    }
                    continue;
                }
                auto permit = String(request, "permit");
                auto conversation = std::find_if(workerJobs.begin(), workerJobs.end(),
                                                 [&](auto const& item) { return item.permit == permit; });
                if (conversation != workerJobs.end())
                {
                    bool const ok = request.at("ok").as_bool();
                    fault = fault || !ok;
                    conversation->state = ok ? "granted" : "denied";
                    continue;
                }
                auto found = std::find_if(attempts.begin(), attempts.end(),
                                          [&](auto const& entry) { return entry.second.id == permit; });
                if (found == attempts.end())
                    continue;
                auto& attempt = found->second;
                bool ok = request.at("ok").as_bool();
                fault = fault || !ok;
                auto const& job = attempt.job;
                if (!paused && ok && realMs < attempt.expires &&
                    coordinator.Authorize(job.jobToken, job.workerId, job.leaseGeneration, permit, gameMs, realMs))
                {
                    attempt.state = "granted";
                    attempt.expires = realMs + 25000;
                }
                else
                    attempt.state = "denied";
                continue;
            }
            id = String(request, "id", 64);
            auto answer = Handle(frame.connection, request, gameMs, realMs);
            transport.Reply(frame.connection,
                            boost::json::serialize(boost::json::object{{"id", id}, {"result", answer}}));
        }
        catch (std::exception const&)
        {
            transport.Reply(frame.connection, boost::json::serialize(boost::json::object{
                                                  {"id", id}, {"error", "invalid or unauthorized request"}}));
        }
    }
    std::erase_if(claims, [&](auto const& entry) { return realMs >= entry.second.admittedRealTimeMs + 45000; });
    std::erase_if(attempts, [&](auto const& entry) { return realMs >= entry.second.job.admittedRealTimeMs + 60000; });
    std::erase_if(interviews, [&](auto const& pair) { return realMs >= pair.second.expires; });
    Reconcile(gameMs, realMs);
    for (auto& [token, attempt] : attempts)
        if (attempt.state == "pending" && realMs >= attempt.expires)
        {
            fault = true;
            attempt.state = "denied";
        }
}

boost::json::object Service::Handle(uint64_t connection, boost::json::object const& request, uint64_t gameMs,
                                    uint64_t realMs)
{
    Fields(request, {"id", "token", "op", "args"});
    auto token = String(request, "token", 128);
    auto op = String(request, "op", 32);
    bool const control = op == "interpreter_status" || op == "interpreter_policy" ||
        op == "interview_submit" || op == "interview_status";
    auto const& secret = control ? settings.controlToken : settings.token;
    // Compare every byte, including when lengths differ. The secret is never included in diagnostics.
    unsigned difference = unsigned(token.size() ^ secret.size());
    for (std::size_t i = 0; i < secret.size(); ++i)
        difference |= secret[i] ^ (i < token.size() ? token[i] : 0);
    if (difference || secret.empty())
        throw std::invalid_argument("authentication");
    auto const& args = request.at("args").as_object();
    if (control)
        return Control(op, args, realMs);
    std::string worker = "connection-" + std::to_string(connection);
    if (op == "worker_hello")
    {
        bool const v2 = args.contains("contractVersion");
        bool const planning = args.contains("planningVersion");
        if (planning)
        {
            if (v2)
            {
                Fields(args, {"profile", "model", "maxInFlight", "timeoutSeconds", "planningVersion",
                    "contractVersion", "maxCallsPerJob"});
                if (Number(args, "contractVersion") != 2 || Number(args, "maxCallsPerJob") != 1)
                    throw std::invalid_argument("unsupported agent contract");
            }
            else
                Fields(args, {"profile", "model", "maxInFlight", "timeoutSeconds", "planningVersion"});
            if (Number(args, "planningVersion") != 1)
                throw std::invalid_argument("unsupported planning version");
        }
        else
            Fields(args, {"profile", "model", "maxInFlight", "timeoutSeconds"});
        if (String(args, "profile") != settings.profile || String(args, "model") != settings.model ||
            (Number(args, "maxInFlight") < 1 || Number(args, "maxInFlight") > (v2 ? 32 : 1)) ||
            Number(args, "timeoutSeconds") != 20)
            throw std::invalid_argument("profile mismatch");
        lastWorkerMs = realMs;
        workers.insert(connection);
        workerCapacity[connection] = Number(args, "maxInFlight");
        if (v2)
            interviewWorkers.insert(connection);
        else
            interviewWorkers.erase(connection);
        if (planning)
            planningWorkers.insert(connection);
        else
            planningWorkers.erase(connection);
        while (workers.size() > 8)
        {
            planningWorkers.erase(*workers.begin());
            interviewWorkers.erase(*workers.begin());
            workerCapacity.erase(*workers.begin());
            workers.erase(workers.begin());
        }
        boost::json::object reply{{"workerId", worker}, {"profile", settings.profile}, {"usedRequests", charged}};
        if (v2)
        {
            reply["contractVersion"] = 2;
            reply["policy"] = PolicyStatus();
            boost::json::array buckets;
            for (auto const& [time, count] : reservations)
                buckets.emplace_back(boost::json::array{time, count});
            reply["reservationBuckets"] = std::move(buckets);
        }
        return reply;
    }
    if (!workers.contains(connection))
        throw std::invalid_argument("hello required");
    lastWorkerMs = realMs;
    if (op == "agent_policy")
    {
        Fields(args, {});
        auto policy = PolicyStatus();
        return {{"run", settings.run}, {"revision", scheduler.Policy().revision}, {"mode", policy.at("mode")},
            {"modelRpm", settings.requestsPerMinute}, {"concurrentCalls", settings.concurrentCalls}};
    }
    if (op == "submit_conversation" || op == "submit_planning" || op == "submit_interview")
    {
        auto const purpose = op == "submit_planning" ? Purpose::Planning :
            op == "submit_interview" ? Purpose::Interview : Purpose::Conversation;
        auto accounting = args;
        if (accounting.contains("callCount"))
        {
            if (Number(accounting, "callCount") > 1 || !accounting.at("usageKnown").is_bool())
                throw std::invalid_argument("unsupported call accounting");
            accounting.erase("callCount");
            accounting.erase("usageKnown");
        }
        Fields(accounting, {"jobToken", "permitId", "response", "outcome", "promptTokens",
            "completionTokens", "latencyMs"});
        auto found = std::find_if(workerJobs.begin(), workerJobs.end(),
                                  [&](auto const& item) { return item.id == String(args, "jobToken", 64); });
        if (found == workerJobs.end() || found->worker != worker || found->state != "running" ||
            found->purpose != purpose ||
            found->permit != String(args, "permitId", 64) || realMs >= found->expires)
            return {{"status", "stale"}};
        auto response = args.at("response").as_object();
        auto const outcome = String(args, "outcome", 32);
        if (found->cancelled)
        {
            if (outcome != "success" && outcome != "failed")
                throw std::invalid_argument("invalid outcome");
            auto const prompt = Number(args, "promptTokens"), completion = Number(args, "completionTokens");
            auto const latency = Number(args, "latencyMs");
            backendCalls += args.contains("callCount") ? Number(args, "callCount") : 1;
            promptTokens += prompt;
            completionTokens += completion;
            serviceMs += latency;
            ObserveLatency(serviceHistogram, latency);
            if (args.contains("usageKnown") && !args.at("usageKnown").as_bool())
                ++unknownUsageJobs;
            if (outcome == "success")
            {
                if (purpose == Purpose::Interview)
                    if (auto receipt = interviews.find(found->id); receipt != interviews.end())
                        receipt->second.result = {{"status", "cancelled"}};
                scheduler.Finish(found->id, "cancelled_completed");
                workerJobs.erase(found);
            }
            else
                found->state = "failed";
            return {{"status", "stale"}}; // Execution ended; cancelled content still cannot reach gameplay.
        }
        if (outcome == "success" && purpose == Purpose::Planning)
            DecodePlanningDecision(response, {});
        else if (outcome == "success" && purpose == Purpose::Interview)
        {
            Fields(response, {"text"});
            if (!IsBoundedText(String(response, "text", 4000), 1000))
                throw std::invalid_argument("invalid interview response");
        }
        else if (outcome == "success")
        {
            Fields(response, {"reply", "text", "action"});
            auto const text = String(response, "text", 255);
            auto const action = String(response, "action", 16);
            bool const reply = response.at("reply").as_bool();
            if (!IsBoundedText(text, 255) || (reply && !IsSafeChatText(text)) ||
                (!reply && (!text.empty() || action != "none")) || text.find('|') != std::string::npos ||
                std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
                (action != "none" && action != "follow" && action != "stop" && action != "assist"
                    && action != "wave" && action != "offer_help"))
                throw std::invalid_argument("invalid conversation response");
        }
        else if (outcome != "failed")
            throw std::invalid_argument("invalid outcome");
        LOG_INFO("module.alles", "Ollama {} {} outcome={} prompt={} completion={} latencyMs={}",
                 purpose == Purpose::Planning ? "planning" : "conversation", found->id, outcome,
                 Number(args, "promptTokens"), Number(args, "completionTokens"), Number(args, "latencyMs"));
        backendCalls += args.contains("callCount") ? Number(args, "callCount") : 1;
        if (args.contains("usageKnown") && !args.at("usageKnown").as_bool())
            ++unknownUsageJobs;
        promptTokens += Number(args, "promptTokens");
        completionTokens += Number(args, "completionTokens");
        serviceMs += Number(args, "latencyMs");
        ObserveLatency(serviceHistogram, Number(args, "latencyMs"));
        if (purpose == Purpose::Interview)
        {
            if (auto receipt = interviews.find(found->id); receipt != interviews.end())
                receipt->second.result = {{"status", outcome}, {"response", response}, {"model", settings.model},
                    {"usageKnown", !args.contains("usageKnown") || args.at("usageKnown").as_bool()},
                    {"promptTokens", Number(args, "promptTokens")},
                    {"completionTokens", Number(args, "completionTokens")}, {"latencyMs", Number(args, "latencyMs")}};
            if (outcome == "success")
            {
                scheduler.Finish(found->id, outcome);
                workerJobs.erase(found);
            }
            else
            {
                found->cancelled = true;
                found->state = "failed";
            }
            return {{"status", "accepted"}};
        }
        auto& results = purpose == Purpose::Planning ? planningResults : conversationResults;
        if (results.size() < 64)
            results.push_back({found->id, outcome, std::move(response)});
        if (outcome == "success")
        {
            ++(purpose == Purpose::Planning ? planningCompleted : conversationCompleted);
            scheduler.State(found->id, Interpreter::JobState::ResultWaiting);
            workerJobs.erase(found);
        }
        else
        {
            ++(purpose == Purpose::Planning ? planningFailed : conversationFailed);
            found->cancelled = true; // Unknown HTTP completion retains capacity until its timeout fence.
            found->state = "failed";
            cooldown = realMs + 5000;
        }
        return {{"status", "accepted"}};
    }
    if (op == "next_jobs")
    {
        Fields(args, {"n"});
        if (Number(args, "n") != 1)
            throw std::invalid_argument("poll one job per request; concurrent execution is negotiated at hello");
        for (auto& item : workerJobs)
            if (!paused && !item.cancelled && item.worker == worker && item.state == "granted")
            {
                item.state = "running";
                scheduler.State(item.id, Interpreter::JobState::Running);
                item.expires = std::min(item.admitted + 45000, realMs + 25000);
                boost::json::object job{{"jobToken", item.id}, {"permitId", item.permit},
                    {"remainingMs", item.expires - realMs}, {"context", item.context}};
                if (item.purpose == Purpose::Planning)
                    job["contractVersion"] = 1;
                return {{item.purpose == Purpose::Planning ? "planning" :
                    item.purpose == Purpose::Interview ? "interview" : "conversation", std::move(job)}};
            }
        auto const owned = std::count_if(scheduler.Jobs().begin(), scheduler.Jobs().end(), [&](auto const& pair)
        {
            if (auto claim = claims.find(pair.first); claim != claims.end())
                return pair.second.state != Interpreter::JobState::Queued && claim->second.workerId == worker;
            return std::any_of(workerJobs.begin(), workerJobs.end(), [&](auto const& item)
                { return item.id == pair.first && item.worker == worker && item.state != "queued"; });
        });
        if (!paused && !fault && Available(epochMs) &&
            realMs >= cooldown && std::size_t(owned) < workerCapacity[connection] &&
            scheduler.Active() < settings.concurrentCalls)
        {
            std::vector<Purpose> supported{Purpose::Conversation};
            if (claims.size() < 256 && attempts.size() < 256)
                supported.push_back(Purpose::Memory);
            if (planningWorkers.contains(connection))
            {
                supported.push_back(Purpose::Planning);
            }
            if (interviewWorkers.contains(connection))
                supported.push_back(Purpose::Interview);
            if (auto selected = scheduler.Select(realMs, supported))
            {
                if (selected->purpose == Purpose::Memory)
                {
                    if (auto job = coordinator.Claim(worker, settings.profile, gameMs, realMs, selected->actor))
                    {
                        scheduler.Reserve(job->jobToken, realMs);
                        totalWaitMs += realMs - selected->admittedMs;
                        ObserveLatency(waitHistogram, realMs - selected->admittedMs);
                        ++dispatchedJobs;
                        claims[job->jobToken] = *job;
                        return {{"job", EncodeJob(*job, realMs)}};
                    }
                    scheduler.Finish(selected->id, "obsolete");
                }
                else
                {
                    auto next = std::find_if(workerJobs.begin(), workerJobs.end(),
                        [&](auto const& item) { return item.id == selected->id && !item.cancelled; });
                    if (next != workerJobs.end() && scheduler.Reserve(next->id, realMs))
                    {
                        if (next->purpose == Purpose::Interview)
                        {
                            auto personal = interviewContext ? interviewContext(selected->actor) : std::nullopt;
                            if (!personal)
                            {
                                scheduler.Finish(next->id, "owner_unavailable");
                                return {{"job", nullptr}, {"retryMs", 100}};
                            }
                            next->context["personalState"] = std::move(*personal);
                            if (boost::json::serialize(next->context).size() > 10000)
                            {
                                scheduler.Finish(next->id, "context_capacity");
                                return {{"job", nullptr}, {"retryMs", 100}};
                            }
                        }
                        totalWaitMs += realMs - selected->admittedMs;
                        ObserveLatency(waitHistogram, realMs - selected->admittedMs);
                        ++dispatchedJobs;
                        Charge(epochMs);
                        next->worker = worker;
                        next->permit = "agent-" + std::to_string(epochMs) + "-" + std::to_string(charged);
                        next->state = "pending";
                        transport.Reserve(next->permit, boost::json::serialize(boost::json::object{
                            {"profile", settings.profile}, {"permit", next->permit}, {"job", next->id},
                            {"model", settings.model}, {"reservedUnixMs", epochMs}}));
                        return {{"job", nullptr}, {"retryMs", 100}};
                    }
                }
            }
        }
        return {{"job", nullptr}, {"retryMs", 500}, {"budgetExhausted", !Available(epochMs)}, {"ledgerFault", fault}};
    }

    if (op == "submit_result")
    {
        Fields(args, {"proposal"});
        if (paused)
        {
            if (heldResults.size() >= 64)
                return {{"status", "result_capacity"}};
            heldResults.push_back({connection, boost::json::serialize(request)});
            return {{"status", "buffered"}};
        }
        auto result = DecodeProposal(args.at("proposal").as_object());
        auto found = attempts.find(result.jobToken);
        if (result.workerId != worker || found == attempts.end() || found->second.id != result.permitId)
            throw std::invalid_argument("unknown permit");
        auto& attempt = found->second;
        if (attempt.outcome.empty())
        {
            attempt.outcome = coordinator.ApplyExternal(result, gameMs, realMs);
            LOG_INFO("module.alles", "Ollama job {}: {} ({} proposed memories)", result.jobToken, attempt.outcome,
                     result.memories.size());
        }
        if (attempt.complete)
            scheduler.Finish(result.jobToken, attempt.outcome);
        return {{"status", attempt.outcome}};
    }
    if (op == "complete_attempt")
    {
        auto accounting = args;
        if (accounting.contains("callCount"))
        {
            if (Number(accounting, "callCount") > 1 || !accounting.at("usageKnown").is_bool())
                throw std::invalid_argument("unsupported call accounting");
            accounting.erase("callCount");
            accounting.erase("usageKnown");
        }
        Fields(accounting, {"jobToken", "permitId", "outcome", "promptTokens", "completionTokens", "latencyMs"});
        auto found = attempts.find(String(args, "jobToken", 64));
        if (found == attempts.end() || found->second.id != String(args, "permitId", 64) ||
            found->second.job.workerId != worker)
            throw std::invalid_argument("unknown permit");
        auto& attempt = found->second;
        if (!attempt.reported)
        {
            auto outcome = String(args, "outcome", 32);
            if (outcome != "success" && outcome != "failed")
                throw std::invalid_argument("invalid attempt outcome");
            Number(args, "promptTokens");
            Number(args, "completionTokens");
            Number(args, "latencyMs");
            attempt.reported = true;
            if (outcome != "success")
                cooldown = std::max(cooldown, realMs + 30000);
            // Only a fully consumed HTTP response frees capacity early. Timeout/unknown requests
            // retain their slot through the permit lifetime; a disconnect never refunds an attempt.
            backendCalls += args.contains("callCount") ? Number(args, "callCount") : 1;
            if (args.contains("usageKnown") && !args.at("usageKnown").as_bool())
                ++unknownUsageJobs;
            promptTokens += Number(args, "promptTokens");
            completionTokens += Number(args, "completionTokens");
            serviceMs += Number(args, "latencyMs");
            ObserveLatency(serviceHistogram, Number(args, "latencyMs"));
            if (outcome == "success")
                attempt.complete = true;
            LOG_INFO("module.alles", "Ollama attempt {} outcome={} prompt={} completion={} latencyMs={}", attempt.id,
                     outcome, Number(args, "promptTokens"), Number(args, "completionTokens"),
                     Number(args, "latencyMs"));
        }
        return {{"status", "recorded"}};
    }
    Fields(args, {"jobToken", "leaseGeneration", "requestId"});
    auto jobToken = String(args, "jobToken", 64);
    auto lease = Number(args, "leaseGeneration");
    auto requestId = String(args, "requestId", 64);
    if (op == "heartbeat")
        return {{"ok", coordinator.Heartbeat(jobToken, worker, lease, realMs)}};
    if (op == "release_job")
    {
        bool const released = paused || coordinator.Release(jobToken, worker, lease);
        if (!paused && released && !attempts.contains(jobToken))
        {
            scheduler.Finish(jobToken, "released_before_attempt");
            claims.erase(jobToken);
        }
        return {{"ok", released}};
    }
    if (op == "job_status")
    {
        auto found = attempts.find(jobToken);
        if (found == attempts.end() || found->second.job.workerId != worker)
            return {{"status", "unknown"}};
        return {{"status", found->second.outcome.empty() ? found->second.state : found->second.outcome}};
    }
    if (op != "begin_attempt")
        throw std::invalid_argument("unknown operation");
    auto found = attempts.find(jobToken);
    if (found != attempts.end())
    {
        auto const& a = found->second;
        if (a.requestId != requestId || a.job.workerId != worker || a.job.leaseGeneration != lease)
            throw std::invalid_argument("permit request mismatch");
        return {{"status", realMs >= a.expires ? "expired" : a.state}, {"permitId", a.id}};
    }
    auto claim = claims.find(jobToken);
    if (paused || fault || !Available(epochMs) || claim == claims.end() || realMs < cooldown ||
        claim->second.workerId != worker || claim->second.leaseGeneration != lease ||
        realMs + 25000 > claim->second.admittedRealTimeMs + 45000 ||
        !coordinator.Heartbeat(jobToken, worker, lease, realMs))
        return {{"status", "denied"}};
    auto job = coordinator.Inspect(claim->second.owner);
    if (!job || job->jobToken != jobToken)
        return {{"status", "denied"}};
    Charge(epochMs);
    auto permit = job->bootEpoch + ":" + std::to_string(charged);
    attempts.emplace(jobToken, Attempt{*job, permit, requestId, "pending", {}, realMs + 15000, false});
    transport.Reserve(permit, boost::json::serialize(
                                  boost::json::object{{"profile", settings.profile},
                                                      {"permit", permit},
                                                      {"job", jobToken},
                                                      {"request", requestId},
                                                      {"model", settings.model},
                                                      {"reservedUnixMs", epochMs},
                                                      {"estimatedTokens", (job->contextBytesBound + 3) / 4 + 1024}}));
    return {{"status", "pending"}, {"permitId", permit}};
}
} // namespace Alles::Bridge
