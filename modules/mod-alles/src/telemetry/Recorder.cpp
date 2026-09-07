/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Recorder.h"
#include "ObjectGuid.h"
#include "Observatory.h"
#include "Player.h"
#include <algorithm>
#include <atomic>
#include <chrono>

namespace Alles::Telemetry
{
namespace
{
std::atomic<Recorder*> ActiveRecorder{nullptr};

uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LiveSinkTrampoline(Player const* player, std::string_view kind, uint64 value, std::string_view detail,
    std::string_view context)
{
    auto* recorder = ActiveRecorder.load(std::memory_order_acquire);
    if (!recorder || !player)
        return;
    auto const guid = player->GetGUID();
    if (!guid.IsPlayer() || !recorder->IsOwner(guid.GetCounter()))
        return;
    LiveEvent event;
    event.guidLow = guid.GetCounter();
    event.kind = kind;
    event.value = value;
    event.realMs = Now();
    if (kind != "ai_update")
    {
        // Journaled kinds carry the Observatory identity and place; the counted-only kind stays allocation free.
        event.id = guid.ToString();
        event.map = player->GetMapId();
        event.instance = player->GetInstanceId();
        event.detail = detail;
        event.context = context;
    }
    recorder->Live(std::move(event));
}

std::set<uint32_t> PlayerOwners(std::set<ActorKey> const& owners)
{
    std::set<uint32_t> result;
    for (auto const owner : owners)
        if (owner.kind == ActorKind::Player && owner.id <= UINT32_MAX)
            result.insert(uint32_t(owner.id));
    return result;
}
} // namespace

std::string BotIdentity(ActorKey owner)
{
    if (owner.kind != ActorKind::Player || owner.id > UINT32_MAX)
        return "";
    return ObjectGuid(HighGuid::Player, uint32(owner.id)).ToString();
}

std::string TruncateUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return std::string(text);
    auto end = maxBytes;
    // Never cut inside a multi-byte sequence: back up over continuation bytes.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    return std::string(text.substr(0, end));
}

char const* PerceptionName(PerceptionKind kind)
{
    switch (kind)
    {
        case PerceptionKind::Speech: return "speech";
        case PerceptionKind::Emote: return "emote";
        case PerceptionKind::WitnessedDeath: return "witnessed_death";
        case PerceptionKind::OwnDeath: return "own_death";
        case PerceptionKind::Met: return "met";
    }
    return "perception";
}

char const* FormationName(FormationMode mode)
{
    switch (mode)
    {
        case FormationMode::Model: return "model";
        case FormationMode::InProcessFake: return "fake";
        case FormationMode::Reflex: return "reflex";
        case FormationMode::Fallback: return "fallback";
    }
    return "unknown";
}

char const* StateName(ActorState state)
{
    switch (state)
    {
        case ActorState::Loading: return "loading";
        case ActorState::Ready: return "ready";
        case ActorState::Closing: return "closing";
    }
    return "unknown";
}

std::string Describe(Perception const& perception)
{
    std::string text;
    switch (perception.kind)
    {
        case PerceptionKind::Speech:
            text = perception.comprehended ? "\"" + perception.text + "\"" : "unintelligible speech";
            if (!perception.source.name.empty())
                text += " from " + perception.source.name;
            break;
        case PerceptionKind::Emote:
            text = perception.source.name.empty() ? perception.text : perception.source.name + " " + perception.text;
            break;
        case PerceptionKind::WitnessedDeath:
            text = (perception.subject.name.empty() ? "someone" : perception.subject.name) + " died";
            if (!perception.source.name.empty())
                text += ", killed by " + perception.source.name;
            break;
        case PerceptionKind::OwnDeath:
            text = "own death";
            if (!perception.source.name.empty())
                text += ", killed by " + perception.source.name;
            break;
        case PerceptionKind::Met:
            text = "met " + (perception.subject.name.empty() ? std::string("someone") : perception.subject.name);
            break;
    }
    if (!perception.place.empty())
        text += " at " + perception.place;
    return TruncateUtf8(text, 240);
}

void InstallLiveSink(Recorder* recorder)
{
    if (recorder)
    {
        ActiveRecorder.store(recorder, std::memory_order_release);
        Observatory::SetLiveSink(&LiveSinkTrampoline);
        return;
    }
    Observatory::SetLiveSink(nullptr);
    ActiveRecorder.store(nullptr, std::memory_order_release);
}

Recorder::Recorder(std::filesystem::path directory, std::string run, std::set<ActorKey> const& owners,
    uint64_t segmentBytes, uint64_t startedRealMs)
    : _run(std::move(run)), _started(startedRealMs), _owners(PlayerOwners(owners)),
      _events(directory, "events.ndjson", segmentBytes, 8192),
      _snapshots(directory, "snapshots.ndjson", segmentBytes, 64)
{
    for (auto const low : _owners)
        _counters.emplace(low, BotCounters{});
}

Recorder::~Recorder()
{
    if (ActiveRecorder.load(std::memory_order_acquire) == this)
        InstallLiveSink(nullptr);
}

bool Recorder::IsOwner(uint32_t guidLow) const
{
    return _owners.contains(guidLow);
}

bool Recorder::Live(LiveEvent event)
{
    if (!_owners.contains(event.guidLow))
        return true;
    std::lock_guard lock(_mutex);
    auto& counters = _counters[event.guidLow];
    if (event.kind == "ai_update")
    {
        ++counters.aiUpdates;
        counters.lastAiMs = event.realMs;
        return true;
    }
    if (event.kind == "xp")
        counters.xp += event.value;
    else if (event.kind == "death")
        ++counters.deaths;
    else if (event.kind == "quest_reward")
        ++counters.quests;
    else if (event.kind == "bot_action")
    {
        ++counters.actions;
        counters.lastAction = event.detail;
        counters.lastActionMs = event.realMs;
    }
    if (_queue.size() >= _queueLimit)
    {
        ++_liveDropped;
        return false;
    }
    _queue.push_back(std::move(event));
    return true;
}

std::size_t Recorder::Drain(std::size_t budget)
{
    std::vector<LiveEvent> batch;
    {
        std::lock_guard lock(_mutex);
        auto const count = std::min(budget, _queue.size());
        batch.assign(std::make_move_iterator(_queue.begin()), std::make_move_iterator(_queue.begin() + count));
        _queue.erase(_queue.begin(), _queue.begin() + count);
    }
    for (auto const& event : batch)
        Emit(event.id, event.kind, event.value, event.detail, event.context, event.map, event.instance, event.realMs);
    return batch.size();
}

void Recorder::Emit(std::string const& id, std::string_view kind, uint64_t value, std::string_view detail,
    std::string_view context, uint32_t map, uint32_t instance, uint64_t realMs)
{
    boost::json::object record{{"run", _run}, {"seq", ++_sequence}, {"simMs", SimMs(realMs)}, {"bot", id},
        {"kind", kind}, {"value", value}, {"detail", detail}, {"context", context}, {"map", map},
        {"instance", instance}};
    _events.Append(boost::json::serialize(record));
}

void Recorder::RecordPerception(ActorKey owner, Perception const& perception, bool admitted, uint64_t realMs)
{
    Emit(BotIdentity(owner), "alles_perception", admitted ? 1 : 0, Describe(perception),
        PerceptionName(perception.kind), 0, 0, realMs);
}

void Recorder::RecordSpeech(ActorKey owner, std::string_view line, bool delivered, uint64_t realMs)
{
    Emit(BotIdentity(owner), "alles_said", delivered ? 1 : 0, TruncateUtf8(line, 300),
        delivered ? "delivered" : "no listener", 0, 0, realMs);
}

void Recorder::RecordOwner(ActorKey owner, std::optional<OwnerStatus> const& status, OwnerSnapshot const* snapshot,
    uint64_t realMs)
{
    auto& track = _tracks[owner];
    auto const id = BotIdentity(owner);
    std::optional<ActorState> const state = status ? std::optional(status->state) : std::nullopt;
    if (state != track.state)
    {
        Emit(id, "alles_owner", state ? uint64_t(*state) : 0, state ? StateName(*state) : "unloaded", "memory store",
            0, 0, realMs);
        track.state = state;
    }
    if (status)
    {
        if (status->committedRevision > track.committedRevision)
        {
            Emit(id, "alles_save", status->committedRevision, "committed", "memory store", 0, 0, realMs);
            track.committedRevision = status->committedRevision;
        }
        if (status->saveFailed && !track.saveFailed)
            Emit(id, "alles_save", status->revision, "failed", "memory store", 0, 0, realMs);
        track.saveFailed = status->saveFailed;
        if (status->generation != track.generation)
        {
            // A fresh load replays persisted memories; they are history, not formations of this moment.
            track.generation = status->generation;
            track.seen = false;
        }
    }
    if (!snapshot)
        return;
    std::map<uint64_t, uint64_t> revisions;
    uint64_t highest = track.maxMemoryId;
    for (auto const& memory : snapshot->memories)
    {
        highest = std::max(highest, memory.id);
        revisions.emplace(memory.id, memory.contentRevision);
        if (!track.seen)
            continue;
        if (memory.id > track.maxMemoryId)
            Emit(id, "alles_memory", memory.id, TruncateUtf8(RenderMemory(memory), 300),
                FormationName(memory.formation), 0, 0, realMs);
        else if (auto found = track.revisions.find(memory.id);
                 found != track.revisions.end() && found->second != memory.contentRevision)
            Emit(id, "alles_memory", memory.id, TruncateUtf8(RenderMemory(memory), 300), "revised", 0, 0, realMs);
    }
    track.maxMemoryId = highest;
    track.revisions = std::move(revisions);
    track.seen = true;
}

void Recorder::RecordInterpreter(bool connected, uint64_t usedRequests, uint64_t realMs)
{
    if (_connected != connected)
    {
        Emit("", "alles_worker", connected ? 1 : 0, connected ? "connected" : "disconnected", "interpreter", 0, 0,
            realMs);
        _connected = connected;
    }
    if (_usedRequests && usedRequests > *_usedRequests)
        Emit("", "alles_request", usedRequests, "pilot request charged", "interpreter", 0, 0, realMs);
    _usedRequests = usedRequests;
}

void Recorder::Record(ActorKey owner, std::string_view kind, uint64_t value, std::string_view detail,
    std::string_view context, uint64_t realMs)
{
    Emit(BotIdentity(owner), kind, value, TruncateUtf8(detail, 300), context, 0, 0, realMs);
}

void Recorder::RecordSnapshot(std::string line)
{
    _snapshots.Append(std::move(line));
}

void Recorder::Flush()
{
    _events.Flush();
    _snapshots.Flush();
}

std::optional<BotCounters> Recorder::Counters(ActorKey owner) const
{
    if (owner.kind != ActorKind::Player || owner.id > UINT32_MAX)
        return std::nullopt;
    std::lock_guard lock(_mutex);
    auto const found = _counters.find(uint32_t(owner.id));
    if (found == _counters.end())
        return std::nullopt;
    return found->second;
}

uint32_t Recorder::ActiveBots(std::set<uint32_t> const& online, uint64_t realMs, uint64_t windowMs) const
{
    std::lock_guard lock(_mutex);
    uint32_t active = 0;
    for (auto const& [low, counters] : _counters)
        if (online.contains(low) && counters.aiUpdates && realMs >= counters.lastAiMs
            && realMs - counters.lastAiMs < windowMs)
            ++active;
    return active;
}

RunTotals Recorder::Totals() const
{
    std::lock_guard lock(_mutex);
    RunTotals totals;
    for (auto const& [low, counters] : _counters)
    {
        totals.xp += counters.xp;
        totals.deaths += counters.deaths;
        totals.quests += counters.quests;
    }
    return totals;
}

uint64_t Recorder::SimMs(uint64_t realMs) const
{
    return realMs >= _started ? realMs - _started : 0;
}

boost::json::object Recorder::Status() const
{
    auto describe = [](JournalStatus const& status)
    {
        return boost::json::object{{"records", status.records}, {"dropped", status.dropped},
            {"bytes", status.bytes}, {"segments", status.segments}, {"failed", status.failed}};
    };
    uint64_t liveDropped;
    {
        std::lock_guard lock(_mutex);
        liveDropped = _liveDropped;
    }
    return {{"events", describe(_events.Status())}, {"snapshots", describe(_snapshots.Status())},
        {"liveDropped", liveDropped}};
}
}
