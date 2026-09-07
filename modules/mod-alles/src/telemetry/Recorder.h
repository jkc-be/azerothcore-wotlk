/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_TELEMETRY_RECORDER_H
#define MOD_ALLES_TELEMETRY_RECORDER_H

#include "Journal.h"
#include "domain/ActorStore.h"
#include <boost/json.hpp>
#include <map>
#include <optional>
#include <set>

class Player;

namespace Alles::Telemetry
{
// Per-bot counters with the same meaning as the Observatory snapshot fields they feed.
struct BotCounters
{
    uint64_t aiUpdates = 0;     // PlayerbotAI::UpdateAI calls
    uint64_t lastAiMs = 0;      // steady-clock ms of the last AI update, 0 when never updated
    uint64_t actions = 0;       // completed bot actions (Observatory `bot_action`)
    uint64_t lastActionMs = 0;
    std::string lastAction;
    uint64_t xp = 0;
    uint64_t deaths = 0;
    uint64_t quests = 0;
};

struct RunTotals
{
    uint64_t xp = 0;
    uint64_t deaths = 0;
    uint64_t quests = 0;
};

// One Observatory::Event record copied out of the raising thread. `id` is the Observatory bot identity
// (ObjectGuid::ToString()) and stays empty for counted-only kinds such as `ai_update`.
struct LiveEvent
{
    uint32_t guidLow = 0;
    std::string id;
    uint32_t map = 0;
    uint32_t instance = 0;
    std::string kind;
    uint64_t value = 0;
    std::string detail;
    std::string context;
    uint64_t realMs = 0;
};

// Ordinary-realm telemetry for the configured owners: the journal the Observatory dashboard reads
// (`events.ndjson`, `snapshots.ndjson`) plus the per-bot AI and progression counters the snapshot carries.
// The tap runs on any thread and only copies values; everything else is world-thread only.
class Recorder
{
public:
    Recorder(std::filesystem::path directory, std::string run, std::set<ActorKey> const& owners,
        uint64_t segmentBytes, uint64_t startedRealMs);
    ~Recorder();
    Recorder(Recorder const&) = delete;
    Recorder& operator=(Recorder const&) = delete;

    bool IsOwner(uint32_t guidLow) const;
    // Any thread. Counts AI updates and progression; queues journaled kinds. False when the queue dropped it.
    bool Live(LiveEvent event);

    // World thread. Writes queued live records to the journal in arrival order.
    std::size_t Drain(std::size_t budget = 512);
    void RecordPerception(ActorKey owner, Perception const& perception, bool admitted, uint64_t realMs);
    void RecordSpeech(ActorKey owner, std::string_view line, bool delivered, uint64_t realMs);
    // Emits memory formation, revision, store state and save outcome changes since the previous sample.
    void RecordOwner(ActorKey owner, std::optional<OwnerStatus> const& status, OwnerSnapshot const* snapshot,
        uint64_t realMs);
    void RecordInterpreter(bool connected, uint64_t usedRequests, uint64_t realMs);
    // Any other module event about an owner (for example a delivered conversation turn); use an `alles_` kind.
    void Record(ActorKey owner, std::string_view kind, uint64_t value, std::string_view detail,
        std::string_view context, uint64_t realMs);
    void RecordSnapshot(std::string line);
    void Flush();

    std::optional<BotCounters> Counters(ActorKey owner) const;
    // Bots whose AI updated within the window, the Observatory's `activeBots` definition.
    uint32_t ActiveBots(uint64_t realMs, uint64_t windowMs = 10000) const;
    RunTotals Totals() const;
    uint64_t SimMs(uint64_t realMs) const;
    boost::json::object Status() const;

private:
    struct OwnerTrack
    {
        bool seen = false;
        uint64_t generation = 0;
        uint64_t maxMemoryId = 0;
        std::map<uint64_t, uint64_t> revisions;
        std::optional<ActorState> state;
        uint64_t committedRevision = 0;
        bool saveFailed = false;
    };

    void Emit(std::string const& id, std::string_view kind, uint64_t value, std::string_view detail,
        std::string_view context, uint32_t map, uint32_t instance, uint64_t realMs);

    std::string const _run;
    uint64_t const _started;
    std::set<uint32_t> const _owners;
    std::size_t const _queueLimit = 4096;
    Journal _events;
    Journal _snapshots;
    mutable std::mutex _mutex;
    std::map<uint32_t, BotCounters> _counters;
    std::deque<LiveEvent> _queue;
    uint64_t _liveDropped = 0;
    uint64_t _sequence = 0;
    std::map<ActorKey, OwnerTrack> _tracks;
    std::optional<bool> _connected;
    std::optional<uint64_t> _usedRequests;
};

std::string BotIdentity(ActorKey owner);
std::string Describe(Perception const& perception);
std::string TruncateUtf8(std::string_view text, std::size_t maxBytes);
char const* PerceptionName(PerceptionKind kind);
char const* FormationName(FormationMode mode);
char const* StateName(ActorState state);

// World thread. Routes Observatory::Event records to `recorder`; nullptr clears the tap. Clearing does not
// wait for a sink call already running on a map thread, so clear it only after maps have unloaded.
void InstallLiveSink(Recorder* recorder);
}

#endif
