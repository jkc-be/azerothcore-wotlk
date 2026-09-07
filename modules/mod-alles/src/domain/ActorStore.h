/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_ACTOR_STORE_H
#define MOD_ALLES_ACTOR_STORE_H

#include "Memory.h"
#include <deque>
#include <map>
#include <vector>

namespace Alles
{
enum class ActorState : uint8_t
{
    Loading,
    Ready,
    Closing
};

struct OwnerSnapshot
{
    ActorKey owner;
    uint64_t revision = 0;
    uint64_t nextPerceptionId = 1;
    uint64_t nextMemoryId = 1;
    uint64_t decayGameTimeMs = 0;
    uint64_t nextConsolidationGameTimeMs = 0;
    std::vector<Perception> perceptions;
    std::vector<Memory> memories;
};

struct SaveRequest
{
    uint64_t generation = 0;
    OwnerSnapshot snapshot;
};

struct MemoryVersion
{
    uint64_t id;
    uint64_t contentRevision;
};

enum class MemoryMutationKind : uint8_t
{
    Create,
    Reinforce,
    Supersede
};

struct MemoryTarget
{
    ActorKey owner;
    MemoryVersion version;
};

struct MemoryMutation
{
    MemoryMutationKind kind = MemoryMutationKind::Create;
    std::optional<MemoryTarget> target;
    Memory memory;
};

struct StoreLimits
{
    std::size_t owners = 256;
    std::size_t memories = 256;
    std::size_t perceptions = 128;
    std::size_t inFlightSaves = 4;
};

struct OwnerStatus
{
    ActorState state;
    uint64_t generation;
    uint64_t attachment;
    uint64_t revision;
    uint64_t committedRevision;
    bool saveFailed;
    bool saving;
    uint64_t droppedPerceptions;
};

bool IsValidSnapshot(OwnerSnapshot const& snapshot, StoreLimits const& limits, MemoryPolicy const& policy);

// World-thread value store. DB/hook callbacks must enqueue values before calling this class.
// No runtime gameplay pointer, DB handle, or cross-owner memory lookup enters formation.
class ActorStore
{
public:
    explicit ActorStore(StoreLimits limits = {}, MemoryPolicy policy = {});

    // A nonzero attachment identifies one runtime incarnation; zero activates offline pending work.
    // The generation is a load/result fence, independent of runtime attachment changes.
    std::optional<uint64_t> Activate(ActorKey owner, uint64_t attachment);
    bool FinishLoad(ActorKey owner, uint64_t generation, OwnerSnapshot snapshot,
        uint64_t gameTimeMs, uint64_t realTimeMs);
    void Close(ActorKey owner, uint64_t attachment);
    void RequestFlush(ActorKey owner);

    // During Loading this only accepts a bounded ingress value; merge admission can still drop it
    // if persisted inputs fill the pending cap. Such drops are exposed in OwnerStatus.
    bool Observe(ActorKey owner, Perception perception, uint64_t realTimeMs);
    bool Apply(ActorKey owner, uint64_t generation, std::vector<uint64_t> const& perceptionIds,
        std::vector<MemoryVersion> const& expectedMemories, std::vector<Memory> memories,
        uint64_t gameTimeMs, uint64_t realTimeMs);
    // Validates and stages the entire prefix/mutation transaction before publishing any changes.
    // Targets must name this typed owner and a supplied, still-current memory content revision.
    // The coordinator grounds supports/kind/provenance before constructing these internal values.
    bool ApplyMutations(ActorKey owner, uint64_t generation, std::vector<uint64_t> const& perceptionIds,
        std::vector<MemoryVersion> const& expectedMemories, std::vector<MemoryMutation> const& mutations,
        uint64_t gameTimeMs, uint64_t realTimeMs);
    void Decay(uint64_t gameTimeMs, uint64_t realTimeMs);
    bool Rehearse(ActorKey owner, uint64_t memoryId, uint64_t gameTimeMs, uint64_t realTimeMs, double strength);

    std::optional<SaveRequest> CaptureSave(ActorKey owner, uint64_t realTimeMs);
    // Shutdown only, after stopping async admission and consuming older write outcomes. Bypasses
    // ordinary timing/retry delay, while retaining the per-owner and global in-flight fences.
    std::optional<SaveRequest> CaptureFinalSave(ActorKey owner, uint64_t realTimeMs);
    std::vector<SaveRequest> CaptureDueSaves(uint64_t realTimeMs, std::size_t ownerBudget = 32);
    bool CompleteSave(ActorKey owner, uint64_t generation, uint64_t revision, bool success, uint64_t realTimeMs);
    bool EvictClosed(ActorKey owner);
    std::optional<OwnerStatus> Status(ActorKey owner) const;
    OwnerSnapshot const* FindReady(ActorKey owner) const;

private:
    struct SpeechReceipt
    {
        Reference source;
        PerceptionKind kind;
        uint32_t language;
        bool comprehended;
        std::string text;
        uint64_t gameTimeMs;
    };

    struct Entry
    {
        ActorState state = ActorState::Loading;
        uint64_t generation = 0;
        uint64_t attachment = 0;
        OwnerSnapshot snapshot;
        std::deque<Perception> loadingBuffer;
        std::deque<SpeechReceipt> speechReceipts;
        std::deque<std::pair<uint64_t, uint64_t>> emissionReceipts;
        std::optional<SaveRequest> inFlight;
        uint64_t committedRevision = 0;
        uint64_t dirtySinceMs = 0;
        uint64_t lastSaveMs = 0;
        uint64_t retryAfterMs = 0;
        uint64_t materialChanges = 0;
        uint64_t droppedPerceptions = 0;
        bool flushRequested = false;
        bool saveFailed = false;
    };

    bool IsDuplicate(Entry const& entry, Perception const& perception) const;
    void RememberSpeech(Entry& entry, Perception const& perception);
    void MarkDirty(Entry& entry, uint64_t realTimeMs, std::size_t materialChanges);
    void DecayEntry(Entry& entry, uint64_t gameTimeMs, uint64_t realTimeMs);
    std::optional<SaveRequest> CaptureSnapshot(ActorKey owner, uint64_t realTimeMs, bool final);

    StoreLimits _limits;
    MemoryPolicy _policy;
    std::map<ActorKey, Entry> _owners;
    uint64_t _nextGeneration = 1;
    std::size_t _inFlightSaves = 0;
    std::optional<ActorKey> _saveCursor;
};
}

#endif
