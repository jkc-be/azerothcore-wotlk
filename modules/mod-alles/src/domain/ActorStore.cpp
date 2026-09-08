/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "ActorStore.h"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace Alles
{
namespace
{
constexpr uint64_t SpeechWindowMs = 30000;
constexpr uint64_t OrdinaryFlushMs = 30000;
constexpr uint64_t MinimumFlushMs = 5000;
constexpr uint64_t ChangeThreshold = 64;

bool WithinWindow(uint64_t now, uint64_t first)
{
    return now < first || now - first < SpeechWindowMs;
}

bool SameSource(Reference const& left, Reference const& right)
{
    if (left.actor || right.actor)
        return left.actor == right.actor;
    return left.name == right.name;
}
}

ActorStore::ActorStore(StoreLimits limits, MemoryPolicy policy) : _limits(limits), _policy(policy)
{
    if (!limits.owners || !limits.memories || !limits.perceptions || !limits.inFlightSaves || !IsValidPolicy(policy))
        throw std::invalid_argument("Invalid alles store limits or memory policy");
}

std::optional<uint64_t> ActorStore::Activate(ActorKey owner, uint64_t attachment)
{
    if (!IsValidActor(owner))
        return std::nullopt;

    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
    {
        if (_owners.size() >= _limits.owners || _nextGeneration == std::numeric_limits<uint64_t>::max())
            return std::nullopt;
        iterator = _owners.emplace(owner, Entry{}).first;
        iterator->second.snapshot.owner = owner;
        iterator->second.generation = _nextGeneration++;
    }

    auto& entry = iterator->second;
    if (attachment)
        entry.attachment = attachment;
    if (entry.state == ActorState::Closing && attachment)
        entry.state = ActorState::Ready;
    return entry.generation;
}

bool IsValidSnapshot(OwnerSnapshot const& snapshot, StoreLimits const& limits, MemoryPolicy const& policy)
{
    if (!IsValidPolicy(policy) || !IsValidActor(snapshot.owner) || !snapshot.nextMemoryId || !snapshot.nextPerceptionId
        || snapshot.memories.size() > limits.memories || snapshot.perceptions.size() > limits.perceptions
        || snapshot.revision == std::numeric_limits<uint64_t>::max()
        || (snapshot.planning && (snapshot.planning->owner != snapshot.owner
            || snapshot.planning->revision > snapshot.revision || !IsValidPlanningSnapshot(*snapshot.planning))))
        return false;

    std::set<uint64_t> ids;
    for (auto const& memory : snapshot.memories)
        if (!memory.id || memory.id >= snapshot.nextMemoryId || !IsValidMemory(memory)
            || !ids.insert(memory.id).second
            || (memory.kind == MemoryKind::HeardStatement && memory.confidence > policy.hearsayCap))
            return false;

    uint64_t previousPerceptionId = 0;
    for (auto const& perception : snapshot.perceptions)
    {
        auto gated = perception;
        if (!perception.id || perception.id >= snapshot.nextPerceptionId || !GatePerception(gated)
            || gated.text != perception.text || perception.id <= previousPerceptionId)
            return false;
        previousPerceptionId = perception.id;
    }
    return true;
}

bool ActorStore::FinishLoad(ActorKey owner, uint64_t generation, OwnerSnapshot snapshot,
    uint64_t gameTimeMs, uint64_t realTimeMs)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end() || iterator->second.state != ActorState::Loading
        || iterator->second.generation != generation || snapshot.owner != owner
        || !IsValidSnapshot(snapshot, _limits, _policy))
        return false;

    auto& entry = iterator->second;
    entry.snapshot = std::move(snapshot);
    entry.committedRevision = entry.snapshot.revision;
    entry.state = entry.attachment ? ActorState::Ready : ActorState::Closing;
    // Rebuild admission receipts from retained inputs; ingress dropped during merge must not
    // suppress a later observation after room becomes available.
    entry.speechReceipts.clear();
    entry.emissionReceipts.clear();
    // Monotonic admission timestamps cannot survive a process restart. Restored pending inputs start here.
    for (auto& perception : entry.snapshot.perceptions)
    {
        perception.admittedRealTimeMs = realTimeMs;
        perception.emissionId = 0;
        RememberSpeech(entry, perception);
    }
    DecayEntry(entry, gameTimeMs, realTimeMs);

    auto buffered = std::move(entry.loadingBuffer);
    entry.loadingBuffer.clear();
    for (auto& perception : buffered)
    {
        // The live buffer already passed admission, but may duplicate a restored pending input.
        if (entry.snapshot.perceptions.size() >= _limits.perceptions
            || entry.snapshot.nextPerceptionId == std::numeric_limits<uint64_t>::max())
        {
            ++entry.droppedPerceptions;
            continue;
        }
        auto const& pending = entry.snapshot.perceptions;
        bool const persistedDuplicate = std::any_of(pending.begin(), pending.end(),
            [&perception](Perception const& existing)
            {
                return existing.kind == perception.kind && SameSource(existing.source, perception.source)
                    && existing.language == perception.language && existing.comprehended == perception.comprehended
                    && existing.text == perception.text && existing.kind == PerceptionKind::Speech
                    && WithinWindow(perception.gameTimeMs, existing.gameTimeMs);
            });
        if (persistedDuplicate)
            continue;
        RememberSpeech(entry, perception);
        perception.id = entry.snapshot.nextPerceptionId++;
        entry.snapshot.perceptions.push_back(std::move(perception));
        MarkDirty(entry, realTimeMs, true);
    }
    return true;
}

std::optional<uint64_t> ActorStore::UpdatePlanning(ActorKey owner, uint64_t generation, uint64_t expectedRevision,
    ObjectiveSnapshot objectives, KnowledgeSnapshot knowledge, uint64_t realTimeMs)
{
    auto found = _owners.find(owner);
    if (found == _owners.end() || found->second.state == ActorState::Loading
        || found->second.generation != generation || expectedRevision >= std::numeric_limits<uint64_t>::max() - 1
        || found->second.snapshot.revision >= std::numeric_limits<uint64_t>::max() - 1)
        return std::nullopt;
    auto& entry = found->second;
    auto const& previous = entry.snapshot.planning;
    if ((previous ? previous->revision : 0) != expectedRevision)
        return std::nullopt;
    PlanningSnapshot next{owner, expectedRevision + 1, std::move(objectives), std::move(knowledge)};
    if (!IsValidPlanningSnapshot(next))
        return std::nullopt;
    if (previous && previous->objectives == next.objectives && previous->knowledge == next.knowledge)
        return previous->revision;
    entry.snapshot.planning = std::move(next);
    MarkDirty(entry, realTimeMs, 1);
    return entry.snapshot.planning->revision;
}

void ActorStore::Close(ActorKey owner, uint64_t attachment)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
        return;

    auto& entry = iterator->second;
    RequestFlush(owner);
    if (entry.attachment != attachment)
        return;
    entry.attachment = 0;
    if (entry.state == ActorState::Ready)
        entry.state = ActorState::Closing;
}

void ActorStore::RequestFlush(ActorKey owner)
{
    if (auto iterator = _owners.find(owner); iterator != _owners.end())
    {
        auto& entry = iterator->second;
        if (entry.state == ActorState::Loading || (entry.snapshot.revision != entry.committedRevision
            && (!entry.inFlight || entry.inFlight->snapshot.revision != entry.snapshot.revision)))
            entry.flushRequested = true;
    }
}

bool ActorStore::IsDuplicate(Entry const& entry, Perception const& perception) const
{
    if (perception.emissionId && std::any_of(entry.emissionReceipts.begin(), entry.emissionReceipts.end(),
        [&perception](auto const& receipt)
        {
            return receipt.first == perception.emissionId && WithinWindow(perception.gameTimeMs, receipt.second);
        }))
        return true;

    if (perception.kind != PerceptionKind::Speech && perception.kind != PerceptionKind::Emote)
        return false;
    return std::any_of(entry.speechReceipts.begin(), entry.speechReceipts.end(),
        [&perception](SpeechReceipt const& receipt)
        {
            return SameSource(receipt.source, perception.source) && receipt.kind == perception.kind
                && receipt.language == perception.language && receipt.comprehended == perception.comprehended
                && receipt.text == perception.text && WithinWindow(perception.gameTimeMs, receipt.gameTimeMs);
        });
}

void ActorStore::RememberSpeech(Entry& entry, Perception const& perception)
{
    std::erase_if(entry.emissionReceipts, [&perception](auto const& receipt)
    {
        return !WithinWindow(perception.gameTimeMs, receipt.second);
    });
    std::erase_if(entry.speechReceipts, [&perception](SpeechReceipt const& receipt)
    {
        return !WithinWindow(perception.gameTimeMs, receipt.gameTimeMs);
    });
    if (perception.emissionId)
    {
        entry.emissionReceipts.emplace_back(perception.emissionId, perception.gameTimeMs);
        if (entry.emissionReceipts.size() > _limits.perceptions * 2)
            entry.emissionReceipts.pop_front();
    }
    if (perception.kind == PerceptionKind::Speech || perception.kind == PerceptionKind::Emote)
    {
        entry.speechReceipts.push_back({perception.source, perception.kind, perception.language,
            perception.comprehended, perception.text, perception.gameTimeMs});
        if (entry.speechReceipts.size() > _limits.perceptions)
            entry.speechReceipts.pop_front();
    }
}

bool ActorStore::Observe(ActorKey owner, Perception perception, uint64_t realTimeMs)
{
    if (!GatePerception(perception))
        return false;
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
        return false;
    auto& entry = iterator->second;
    if (perception.kind == PerceptionKind::Speech && perception.source.actor == owner)
        return false;
    if (IsDuplicate(entry, perception))
    {
        // A coalesced repeat still owns its emission ID across locale renderings. Do not refresh
        // the content receipt: its fixed window always starts at the first retained line.
        if (perception.emissionId && std::none_of(entry.emissionReceipts.begin(), entry.emissionReceipts.end(),
            [&perception](auto const& receipt) { return receipt.first == perception.emissionId; }))
        {
            entry.emissionReceipts.emplace_back(perception.emissionId, perception.gameTimeMs);
            if (entry.emissionReceipts.size() > _limits.perceptions * 2)
                entry.emissionReceipts.pop_front();
        }
        return false;
    }

    perception.admittedRealTimeMs = realTimeMs;
    if (entry.state == ActorState::Loading)
    {
        if (entry.loadingBuffer.size() >= _limits.perceptions)
        {
            ++entry.droppedPerceptions;
            return false;
        }
        RememberSpeech(entry, perception);
        entry.loadingBuffer.push_back(std::move(perception));
        return true;
    }
    // Never evict an admitted input: a coordinator may have an immutable job referring to it.
    if (entry.snapshot.perceptions.size() >= _limits.perceptions
        || entry.snapshot.nextPerceptionId == std::numeric_limits<uint64_t>::max())
    {
        ++entry.droppedPerceptions;
        return false;
    }
    RememberSpeech(entry, perception);
    perception.id = entry.snapshot.nextPerceptionId++;
    entry.snapshot.perceptions.push_back(std::move(perception));
    MarkDirty(entry, realTimeMs, true);
    return true;
}

void ActorStore::MarkDirty(Entry& entry, uint64_t realTimeMs, std::size_t materialChanges)
{
    if (entry.snapshot.revision == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("Alles snapshot revision exhausted");
    if (entry.snapshot.revision == entry.committedRevision
        || (entry.inFlight && entry.snapshot.revision == entry.inFlight->snapshot.revision))
        entry.dirtySinceMs = realTimeMs;
    ++entry.snapshot.revision;
    entry.materialChanges += materialChanges;
}

bool ActorStore::Apply(ActorKey owner, uint64_t generation, std::vector<uint64_t> const& perceptionIds,
    std::vector<MemoryVersion> const& expectedMemories, std::vector<Memory> memories,
    uint64_t gameTimeMs, uint64_t realTimeMs)
{
    std::vector<MemoryMutation> mutations;
    for (auto& memory : memories)
        mutations.push_back({MemoryMutationKind::Create, std::nullopt, std::move(memory)});
    return ApplyMutations(owner, generation, perceptionIds, expectedMemories, mutations, gameTimeMs, realTimeMs);
}

bool ActorStore::ApplyMutations(ActorKey owner, uint64_t generation, std::vector<uint64_t> const& perceptionIds,
    std::vector<MemoryVersion> const& expectedMemories, std::vector<MemoryMutation> const& mutations,
    uint64_t gameTimeMs, uint64_t realTimeMs)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end() || iterator->second.generation != generation
        || iterator->second.state == ActorState::Loading || perceptionIds.empty() || mutations.empty()
        || mutations.size() > 4)
        return false;
    auto& entry = iterator->second;
    auto const& snapshot = entry.snapshot;
    constexpr auto MaxRevision = std::numeric_limits<uint64_t>::max();
    if (perceptionIds.size() > snapshot.perceptions.size() || snapshot.revision >= MaxRevision - 1)
        return false;
    for (std::size_t index = 0; index < perceptionIds.size(); ++index)
        if (snapshot.perceptions[index].id != perceptionIds[index])
            return false;

    std::set<uint64_t> supplied;
    for (auto const& expected : expectedMemories)
        if (!supplied.insert(expected.id).second
            || std::none_of(snapshot.memories.begin(), snapshot.memories.end(), [&expected](Memory const& memory)
                { return memory.id == expected.id && memory.contentRevision == expected.contentRevision; }))
            return false;

    std::size_t creates = 0;
    std::set<uint64_t> targets;
    for (auto const& mutation : mutations)
    {
        if (mutation.kind > MemoryMutationKind::Supersede || !IsValidMemory(mutation.memory)
            || (mutation.memory.kind == MemoryKind::HeardStatement && mutation.memory.confidence > _policy.hearsayCap))
            return false;
        if (mutation.kind == MemoryMutationKind::Create)
        {
            if (mutation.target)
                return false;
            ++creates;
            continue;
        }
        if (!mutation.target || mutation.target->owner != owner
            || !targets.insert(mutation.target->version.id).second
            || std::none_of(expectedMemories.begin(), expectedMemories.end(), [&mutation](MemoryVersion const& expected)
            {
                return expected.id == mutation.target->version.id
                    && expected.contentRevision == mutation.target->version.contentRevision;
            }))
            return false;
        auto const target = std::find_if(snapshot.memories.begin(), snapshot.memories.end(),
            [&mutation](Memory const& memory) { return memory.id == mutation.target->version.id; });
        if (target == snapshot.memories.end() || target->contentRevision == MaxRevision
            || (mutation.kind == MemoryMutationKind::Supersede && target->contentRevision >= MaxRevision - 1))
            return false;
        // Reinforcement preserves the existing belief, not a rewritten or reclassified claim.
        // A newly heard source can rehearse it, but never replaces its retained provenance/confidence.
        if (mutation.kind == MemoryMutationKind::Reinforce
            && (target->claim != mutation.memory.claim || target->kind != mutation.memory.kind
                || target->subject != mutation.memory.subject))
            return false;
    }
    // The maximum counter is the exhausted sentinel; the final usable memory ID is maximum minus one.
    if (creates && creates > MaxRevision - snapshot.nextMemoryId)
        return false;

    // Staging also makes allocation failures or target decay/erosion leave the live store untouched.
    OwnerSnapshot updated = snapshot;
    for (auto const& mutation : mutations)
    {
        if (mutation.kind == MemoryMutationKind::Create)
        {
            auto memory = mutation.memory;
            memory.id = updated.nextMemoryId++;
            memory.contentRevision = 1;
            memory.formedGameTimeMs = gameTimeMs;
            memory.decayGameTimeMs = gameTimeMs;
            memory.recalledGameTimeMs = 0;
            memory.salience = std::min(memory.salience, SalienceCeiling(memory));
            updated.memories.push_back(std::move(memory));
            continue;
        }
        auto target = std::find_if(updated.memories.begin(), updated.memories.end(),
            [&mutation](Memory const& memory) { return memory.id == mutation.target->version.id; });
        auto const expectedRevision = target->contentRevision;
        DecayMemory(*target, _policy, gameTimeMs);
        if (target->contentRevision != expectedRevision || target->salience < _policy.forgetBelow)
            return false; // Time-local erosion cannot be undone by an older interpretation.
        if (mutation.kind == MemoryMutationKind::Reinforce)
        {
            // Interpretation can request rehearsal; its magnitude is a local policy, not an absolute model value.
            constexpr double ReinforcementStrength = 0.1;
            RehearseMemory(*target, _policy, gameTimeMs, ReinforcementStrength);
        }
        else
        {
            auto replacement = mutation.memory;
            replacement.id = target->id;
            replacement.contentRevision = expectedRevision + 1;
            replacement.salience = std::min(target->salience, SalienceCeiling(replacement));
            replacement.formedGameTimeMs = gameTimeMs;
            replacement.decayGameTimeMs = gameTimeMs;
            replacement.recalledGameTimeMs = 0;
            *target = std::move(replacement);
        }
    }
    updated.perceptions.erase(updated.perceptions.begin(), updated.perceptions.begin() + perceptionIds.size());
    std::size_t forgottenCount = 0;
    while (updated.memories.size() > _limits.memories)
    {
        auto const forgotten = std::min_element(updated.memories.begin(), updated.memories.end(),
            [](Memory const& left, Memory const& right)
            {
                return left.salience < right.salience || (left.salience == right.salience && left.id < right.id);
            });
        updated.memories.erase(forgotten);
        ++forgottenCount;
    }
    auto const materialChanges = perceptionIds.size() + mutations.size() + forgottenCount;
    if (!IsValidSnapshot(updated, _limits, _policy) || materialChanges > MaxRevision - entry.materialChanges)
        return false;
    entry.snapshot = std::move(updated);
    MarkDirty(entry, realTimeMs, materialChanges);
    return true;
}

void ActorStore::DecayEntry(Entry& entry, uint64_t gameTimeMs, uint64_t realTimeMs)
{
    if (gameTimeMs <= entry.snapshot.decayGameTimeMs)
        return;
    bool changed = false;
    for (auto& memory : entry.snapshot.memories)
        changed = DecayMemory(memory, _policy, gameTimeMs) || changed;
    auto const erased = std::erase_if(entry.snapshot.memories, [this](Memory const& memory)
    {
        return memory.salience < _policy.forgetBelow;
    });
    entry.snapshot.decayGameTimeMs = gameTimeMs;
    if (changed || erased)
        MarkDirty(entry, realTimeMs, erased);
}

void ActorStore::Decay(uint64_t gameTimeMs, uint64_t realTimeMs)
{
    for (auto& [owner, entry] : _owners)
        // Once offline interpretation drains, keep the final snapshot stable until its save completes.
        // Otherwise each tick dirties it again before EvictClosed can observe the acknowledged revision.
        // The next load or attachment applies elapsed decay before the owner can interpret or speak.
        if (entry.state == ActorState::Ready
            || (entry.state == ActorState::Closing && !entry.snapshot.perceptions.empty()))
            DecayEntry(entry, gameTimeMs, realTimeMs);
}

bool ActorStore::Rehearse(ActorKey owner, uint64_t memoryId, uint64_t gameTimeMs, uint64_t realTimeMs, double strength)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end() || iterator->second.state == ActorState::Loading)
        return false;
    auto& entry = iterator->second;
    for (auto& memory : entry.snapshot.memories)
        if (memory.id == memoryId)
        {
            RehearseMemory(memory, _policy, gameTimeMs, strength);
            MarkDirty(entry, realTimeMs, true);
            return true;
        }
    return false;
}

std::optional<SaveRequest> ActorStore::CaptureSave(ActorKey owner, uint64_t realTimeMs)
{
    return CaptureSnapshot(owner, realTimeMs, false);
}

std::optional<SaveRequest> ActorStore::CaptureFinalSave(ActorKey owner, uint64_t realTimeMs)
{
    return CaptureSnapshot(owner, realTimeMs, true);
}

std::optional<SaveRequest> ActorStore::CaptureSnapshot(ActorKey owner, uint64_t realTimeMs, bool final)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
        return std::nullopt;
    auto& entry = iterator->second;
    if (entry.state == ActorState::Loading || entry.inFlight || _inFlightSaves >= _limits.inFlightSaves
        || entry.snapshot.revision == entry.committedRevision || (!final && realTimeMs < entry.retryAfterMs))
        return std::nullopt;

    bool const ageDue = realTimeMs >= entry.dirtySinceMs && realTimeMs - entry.dirtySinceMs >= OrdinaryFlushMs;
    bool const changesDue = entry.materialChanges >= ChangeThreshold && realTimeMs >= entry.lastSaveMs
        && realTimeMs - entry.lastSaveMs >= MinimumFlushMs;
    if (!final && !entry.flushRequested && !ageDue && !changesDue)
        return std::nullopt;

    entry.inFlight = SaveRequest{entry.generation, entry.snapshot};
    entry.flushRequested = false;
    entry.materialChanges = 0;
    entry.lastSaveMs = realTimeMs;
    ++_inFlightSaves;
    return entry.inFlight;
}

std::vector<SaveRequest> ActorStore::CaptureDueSaves(uint64_t realTimeMs, std::size_t ownerBudget)
{
    std::vector<SaveRequest> requests;
    if (_owners.empty() || _inFlightSaves >= _limits.inFlightSaves)
        return requests;

    auto iterator = _saveCursor ? _owners.upper_bound(*_saveCursor) : _owners.begin();
    auto const candidates = std::min(ownerBudget, _owners.size());
    for (std::size_t examined = 0; examined < candidates && _inFlightSaves < _limits.inFlightSaves; ++examined)
    {
        if (iterator == _owners.end())
            iterator = _owners.begin();
        auto const owner = iterator->first;
        ++iterator;
        _saveCursor = owner;
        if (auto request = CaptureSave(owner, realTimeMs))
            requests.push_back(std::move(*request));
    }
    return requests;
}

bool ActorStore::CompleteSave(ActorKey owner, uint64_t generation, uint64_t revision, bool success, uint64_t realTimeMs)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
        return false;
    auto& entry = iterator->second;
    if (entry.generation != generation || !entry.inFlight || entry.inFlight->snapshot.revision != revision)
        return false;

    --_inFlightSaves;
    entry.inFlight.reset();
    entry.saveFailed = !success;
    if (success)
    {
        entry.committedRevision = revision;
        entry.retryAfterMs = 0;
    }
    else
    {
        entry.flushRequested = true;
        entry.retryAfterMs = realTimeMs + MinimumFlushMs;
    }
    return true;
}

bool ActorStore::EvictClosed(ActorKey owner)
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
        return false;
    auto const& entry = iterator->second;
    if (entry.state != ActorState::Closing || entry.inFlight || entry.saveFailed
        || !entry.snapshot.perceptions.empty() || entry.snapshot.revision != entry.committedRevision)
        return false;
    _owners.erase(iterator);
    return true;
}

std::optional<OwnerStatus> ActorStore::Status(ActorKey owner) const
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end())
        return std::nullopt;
    auto const& entry = iterator->second;
    return OwnerStatus{entry.state, entry.generation, entry.attachment, entry.snapshot.revision,
        entry.committedRevision, entry.saveFailed, entry.inFlight.has_value(), entry.droppedPerceptions};
}

OwnerSnapshot const* ActorStore::FindReady(ActorKey owner) const
{
    auto iterator = _owners.find(owner);
    if (iterator == _owners.end() || iterator->second.state == ActorState::Loading)
        return nullptr;
    return &iterator->second.snapshot;
}
}
