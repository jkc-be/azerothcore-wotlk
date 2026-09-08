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

namespace Alles::Bridge
{
Service::Service(Interpreter::PilotCoordinator& value, Settings config)
    : coordinator(value), settings(std::move(config)),
      transport(settings.port, settings.ledger, settings.profile, settings.maxRequests), charged(transport.Charged())
{
    coordinator.EnableExternal();
    auto previous = transport.RecentReservations();
    reservations.assign(previous.begin(), previous.end());
}

bool Service::Available(uint64_t gameMs) const
{
    if (!settings.requestsPerMinute)
        return charged < settings.maxRequests;
    return std::count_if(reservations.begin(), reservations.end(), [&](uint64_t time)
                         { return gameMs < time || gameMs - time < 60000; }) < settings.requestsPerMinute;
}

void Service::Charge(uint64_t gameMs)
{
    ++charged;
    reservations.push_back(gameMs);
}

bool Service::Busy(uint64_t realMs) const
{
    return std::any_of(attempts.begin(), attempts.end(),
                       [&](auto const& item) { return !item.second.complete && realMs < item.second.expires; }) ||
           std::any_of(workerJobs.begin(), workerJobs.end(),
                       [&](auto const& item) { return item.state != "queued" && realMs < item.expires; });
}

bool Service::QueueWorkerJob(std::string id, boost::json::object context, uint64_t realMs, Purpose purpose)
{
    auto const& results = purpose == Purpose::Planning ? planningResults : conversationResults;
    if (workerJobs.size() >= 32 || results.size() >= 64 || id.empty() || id.size() > 64 ||
        boost::json::serialize(context).size() > 10000 ||
        std::any_of(workerJobs.begin(), workerJobs.end(), [&](auto const& item) { return item.id == id; }))
        return false;
    WorkerJob job{std::move(id), std::move(context), realMs, realMs + 45000};
    job.purpose = purpose;
    workerJobs.push_back(std::move(job));
    return true;
}

bool Service::QueueConversation(std::string id, boost::json::object context, uint64_t realMs)
{
    return QueueWorkerJob(std::move(id), std::move(context), realMs, Purpose::Conversation);
}

bool Service::QueuePlanning(std::string id, boost::json::object context, uint64_t realMs)
{
    return QueueWorkerJob(std::move(id), std::move(context), realMs, Purpose::Planning);
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
    return result;
}

std::vector<PlanningResult> Service::TakePlanning()
{
    std::vector<PlanningResult> result;
    result.swap(planningResults);
    return result;
}

boost::json::object Service::Status() const
{
    return {{"mode", "provider"},
            {"connected", lastWorkerMs && nowMs - lastWorkerMs < 10000},
            {"model", settings.model},
            {"profile", settings.profile},
            {"usedRequests", charged},
            {"maxRequests", settings.maxRequests},
            {"ledgerFault", fault},
            {"modelMemories", coordinator.Stats().modelMemories},
            {"invalidResults", coordinator.Stats().invalidResults},
            {"fallbackMemories", coordinator.Stats().fallbackMemories},
            {"budgetMode", settings.requestsPerMinute ? "rolling" : "trial"},
            {"requestsPerMinute", settings.requestsPerMinute},
            {"remainingRequests",
             settings.requestsPerMinute
                 ? settings.requestsPerMinute - std::min<std::size_t>(settings.requestsPerMinute, reservations.size())
                 : settings.maxRequests - std::min(settings.maxRequests, charged)},
            {"conversationQueued", std::count_if(workerJobs.begin(), workerJobs.end(),
                [](auto const& item) { return item.purpose == Purpose::Conversation; })},
            {"planningQueued", std::count_if(workerJobs.begin(), workerJobs.end(),
                [](auto const& item) { return item.purpose == Purpose::Planning; })},
            {"planningCompleted", planningCompleted},
            {"planningFailed", planningFailed},
            {"conversationCompleted", conversationCompleted},
            {"conversationFailed", conversationFailed}};
}

void Service::Update(uint64_t gameMs, uint64_t realMs)
{
    nowMs = realMs;
    epochMs = gameMs;
    std::erase_if(reservations, [&](uint64_t time) { return gameMs >= time && gameMs - time >= 60000; });
    std::erase_if(workerJobs,
                  [&](WorkerJob const& item)
                  {
                      if (realMs < item.expires && !(item.cancelled && item.state == "queued"))
                          return false;
                      auto& results = item.purpose == Purpose::Planning ? planningResults : conversationResults;
                      if (!item.cancelled && results.size() < 64)
                          results.push_back({item.id, "timeout", {}});
                      if (item.state != "failed")
                          ++(item.purpose == Purpose::Planning ? planningFailed : conversationFailed);
                      return true;
                  });
    for (auto const& frame : transport.Poll())
    {
        boost::json::value id;
        try
        {
            auto request = Parse(frame.text).as_object();
            if (!frame.connection)
            {
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
                if (ok && realMs < attempt.expires &&
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
    // Compare every byte, including when lengths differ. The secret is never included in diagnostics.
    unsigned difference = unsigned(token.size() ^ settings.token.size());
    for (std::size_t i = 0; i < settings.token.size(); ++i)
        difference |= settings.token[i] ^ (i < token.size() ? token[i] : 0);
    if (difference)
        throw std::invalid_argument("authentication");
    auto op = String(request, "op", 32);
    auto const& args = request.at("args").as_object();
    std::string worker = "connection-" + std::to_string(connection);
    if (op == "worker_hello")
    {
        bool const planning = args.contains("planningVersion");
        if (planning)
        {
            Fields(args, {"profile", "model", "maxInFlight", "timeoutSeconds", "planningVersion"});
            if (Number(args, "planningVersion") != 1)
                throw std::invalid_argument("unsupported planning version");
        }
        else
            Fields(args, {"profile", "model", "maxInFlight", "timeoutSeconds"});
        if (String(args, "profile") != settings.profile || String(args, "model") != settings.model ||
            Number(args, "maxInFlight") != 1 || Number(args, "timeoutSeconds") != 20)
            throw std::invalid_argument("profile mismatch");
        lastWorkerMs = realMs;
        workers.insert(connection);
        if (planning)
            planningWorkers.insert(connection);
        else
            planningWorkers.erase(connection);
        while (workers.size() > 8)
        {
            planningWorkers.erase(*workers.begin());
            workers.erase(workers.begin());
        }
        return {{"workerId", worker}, {"profile", settings.profile}, {"usedRequests", charged}};
    }
    if (!workers.contains(connection))
        throw std::invalid_argument("hello required");
    lastWorkerMs = realMs;
    if (op == "submit_conversation" || op == "submit_planning")
    {
        auto const purpose = op == "submit_planning" ? Purpose::Planning : Purpose::Conversation;
        Fields(args, {"jobToken", "permitId", "response", "outcome", "promptTokens", "completionTokens", "latencyMs"});
        auto found = std::find_if(workerJobs.begin(), workerJobs.end(),
                                  [&](auto const& item) { return item.id == String(args, "jobToken", 64); });
        if (found == workerJobs.end() || found->worker != worker || found->state != "running" ||
            found->purpose != purpose || found->cancelled ||
            found->permit != String(args, "permitId", 64) || realMs >= found->expires)
            return {{"status", "stale"}};
        auto response = args.at("response").as_object();
        auto const outcome = String(args, "outcome", 32);
        if (outcome == "success" && purpose == Purpose::Planning)
            DecodePlanningDecision(response, {});
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
        auto& results = purpose == Purpose::Planning ? planningResults : conversationResults;
        if (results.size() < 64)
            results.push_back({found->id, outcome, std::move(response)});
        if (outcome == "success")
        {
            ++(purpose == Purpose::Planning ? planningCompleted : conversationCompleted);
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
            throw std::invalid_argument("pilot has one fleet slot");
        for (auto& item : workerJobs)
            if (!item.cancelled && item.worker == worker && item.state == "granted")
            {
                item.state = "running";
                item.expires = std::min(item.admitted + 45000, realMs + 25000);
                boost::json::object job{{"jobToken", item.id}, {"permitId", item.permit},
                    {"remainingMs", item.expires - realMs}, {"context", item.context}};
                if (item.purpose == Purpose::Planning)
                    job["contractVersion"] = 1;
                return {{item.purpose == Purpose::Planning ? "planning" : "conversation", std::move(job)}};
            }
        bool const busy = Busy(realMs);
        if (!fault && Available(gameMs) && !busy && realMs >= cooldown)
        {
            // Each waiting purpose gets a turn in the shared fleet slot and durable budget.
            for (unsigned offset = 0; offset < 3; ++offset)
            {
                auto const turn = (nextPurpose + offset) % 3;
                if (!turn)
                {
                    if (auto job = coordinator.Claim(worker, settings.profile, gameMs, realMs))
                    {
                        nextPurpose = 1;
                        claims[job->jobToken] = *job;
                        return {{"job", EncodeJob(*job, realMs)}};
                    }
                    continue;
                }
                auto const purpose = turn == 1 ? Purpose::Planning : Purpose::Conversation;
                if (purpose == Purpose::Planning && !planningWorkers.contains(connection))
                    continue;
                auto next = std::find_if(workerJobs.begin(), workerJobs.end(), [&](auto const& item)
                {
                    return item.purpose == purpose && !item.cancelled && item.state == "queued" &&
                        realMs + 25000 <= item.expires;
                });
                if (next == workerJobs.end())
                    continue;
                nextPurpose = (turn + 1) % 3;
                Charge(gameMs);
                next->worker = worker;
                next->permit = (purpose == Purpose::Planning ? "plan-" : "chat-") +
                    std::to_string(gameMs) + "-" + std::to_string(charged);
                next->state = "pending";
                transport.Reserve(next->permit, boost::json::serialize(boost::json::object{
                    {"profile", settings.profile}, {"permit", next->permit}, {"job", next->id},
                    {"model", settings.model}, {"purpose", purpose == Purpose::Planning ? "planning" : "conversation"},
                    {"reservedUnixMs", gameMs}}));
                return {{"job", nullptr}, {"retryMs", 100}};
            }
        }
        return {{"job", nullptr}, {"retryMs", 500}, {"budgetExhausted", !Available(gameMs)}, {"ledgerFault", fault}};
    }
    if (op == "submit_result")
    {
        Fields(args, {"proposal"});
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
        return {{"status", attempt.outcome}};
    }
    if (op == "complete_attempt")
    {
        Fields(args, {"jobToken", "permitId", "outcome", "promptTokens", "completionTokens", "latencyMs"});
        auto found = attempts.find(String(args, "jobToken", 64));
        if (found == attempts.end() || found->second.id != String(args, "permitId", 64) ||
            found->second.job.workerId != worker)
            throw std::invalid_argument("unknown permit");
        auto& attempt = found->second;
        if (!attempt.complete)
        {
            auto outcome = String(args, "outcome", 32);
            if (outcome != "success")
                cooldown = std::max(cooldown, realMs + 30000);
            // Only a fully consumed HTTP response frees capacity early. Timeout/unknown requests
            // retain their slot through the permit lifetime; a disconnect never refunds an attempt.
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
        return {{"ok", coordinator.Release(jobToken, worker, lease)}};
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
    if (fault || !Available(gameMs) || Busy(realMs) || claim == claims.end() || realMs < cooldown ||
        claim->second.workerId != worker || claim->second.leaseGeneration != lease ||
        realMs + 25000 > claim->second.admittedRealTimeMs + 45000 ||
        !coordinator.Heartbeat(jobToken, worker, lease, realMs))
        return {{"status", "denied"}};
    auto job = coordinator.Inspect(claim->second.owner);
    if (!job || job->jobToken != jobToken)
        return {{"status", "denied"}};
    Charge(gameMs);
    auto permit = job->bootEpoch + ":" + std::to_string(charged);
    attempts.emplace(jobToken, Attempt{*job, permit, requestId, "pending", {}, realMs + 15000, false});
    transport.Reserve(permit, boost::json::serialize(
                                  boost::json::object{{"profile", settings.profile},
                                                      {"permit", permit},
                                                      {"job", jobToken},
                                                      {"request", requestId},
                                                      {"model", settings.model},
                                                      {"reservedUnixMs", gameMs},
                                                      {"estimatedTokens", (job->contextBytesBound + 3) / 4 + 1024}}));
    return {{"status", "pending"}, {"permitId", permit}};
}
} // namespace Alles::Bridge
