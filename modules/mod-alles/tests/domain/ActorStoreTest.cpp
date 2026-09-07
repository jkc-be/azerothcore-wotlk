/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "ActorStore.h"
#include "gtest/gtest.h"
#include <limits>

namespace Alles
{
namespace
{
constexpr ActorKey Listener{ActorKind::Player, 42};
constexpr ActorKey Speaker{ActorKind::Player, 43};

Perception Speech(std::string text = "I saw Humanb die", uint64_t gameTimeMs = 1000)
{
    Perception perception;
    perception.source = {Speaker, "Humanc"};
    perception.text = std::move(text);
    perception.gameTimeMs = gameTimeMs;
    return perception;
}

uint64_t Load(ActorStore& store, ActorKey owner = Listener, uint64_t attachment = 1)
{
    auto const generation = store.Activate(owner, attachment);
    EXPECT_TRUE(generation);
    OwnerSnapshot snapshot;
    snapshot.owner = owner;
    EXPECT_TRUE(store.FinishLoad(owner, generation.value_or(0), snapshot, 0, 0));
    return generation.value_or(0);
}

bool FormFirst(ActorStore& store, ActorKey owner, uint64_t generation, uint64_t gameTimeMs = 2000)
{
    auto const* snapshot = store.FindReady(owner);
    if (!snapshot || snapshot->perceptions.empty())
        return false;
    auto const perception = snapshot->perceptions.front();
    return store.Apply(owner, generation, {perception.id}, {}, {FormFallback(perception, {}, gameTimeMs)},
        gameTimeMs, gameTimeMs);
}
}

TEST(AllesActorStore, EqualNumericIdsHaveSeparateOwnersAndSnapshots)
{
    ActorStore store;
    auto const playerGeneration = Load(store);
    ActorKey const creature{ActorKind::CreatureSpawn, Listener.id};
    auto const creatureGeneration = Load(store, creature);
    ASSERT_NE(playerGeneration, creatureGeneration);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    ASSERT_TRUE(FormFirst(store, Listener, playerGeneration));
    EXPECT_TRUE(store.FindReady(creature)->memories.empty());
    EXPECT_TRUE(store.FindReady(creature)->perceptions.empty());
    EXPECT_EQ(store.FindReady(Listener)->memories.size(), 1u);
    store.RequestFlush(Listener);
    auto const save = store.CaptureSave(Listener, 2000);
    ASSERT_TRUE(save);
    EXPECT_EQ(save->snapshot.owner, Listener);
    EXPECT_FALSE(store.CaptureSave(creature, 100000));
}

TEST(AllesActorStore, LoadingBuffersGatedInputsWithoutExposingOrFlushingAnEmptyStore)
{
    ActorStore store;
    auto const generation = store.Activate(Listener, 1);
    ASSERT_TRUE(generation);
    auto foreign = Speech("secret in another language");
    foreign.comprehended = false;
    ASSERT_TRUE(store.Observe(Listener, foreign, 1000));
    store.RequestFlush(Listener);
    EXPECT_FALSE(store.FindReady(Listener));
    EXPECT_FALSE(store.CaptureSave(Listener, 100000));

    OwnerSnapshot snapshot;
    snapshot.owner = Listener;
    snapshot.revision = 12;
    snapshot.nextPerceptionId = 9;
    EXPECT_FALSE(store.FinishLoad(Listener, *generation + 1, snapshot, 1000, 1000));
    ASSERT_TRUE(store.FinishLoad(Listener, *generation, snapshot, 1000, 1000));
    auto const* loaded = store.FindReady(Listener);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->perceptions.size(), 1u);
    EXPECT_TRUE(loaded->perceptions.front().text.empty());
    EXPECT_EQ(loaded->perceptions.front().id, 9u);
    EXPECT_GT(loaded->revision, 12u);
    EXPECT_FALSE(store.FinishLoad(Listener, *generation, snapshot, 1000, 1000));
}

TEST(AllesActorStore, LoadRejectsPersistedForeignPlaintextAndCorruptIdentityPairs)
{
    ActorStore store;
    auto const generation = store.Activate(Listener, 1);
    ASSERT_TRUE(generation);
    OwnerSnapshot snapshot;
    snapshot.owner = Listener;
    auto perception = Speech("must never reach fallback");
    perception.id = 1;
    perception.comprehended = false;
    snapshot.perceptions.push_back(perception);
    snapshot.nextPerceptionId = 2;
    EXPECT_FALSE(store.FinishLoad(Listener, *generation, snapshot, 0, 0));
    snapshot.perceptions.front().text.clear();
    snapshot.perceptions.front().source.actor = ActorKey{static_cast<ActorKind>(99), 43};
    EXPECT_FALSE(store.FinishLoad(Listener, *generation, snapshot, 0, 0));
    snapshot.perceptions.front().source.actor = Speaker;
    EXPECT_TRUE(store.FinishLoad(Listener, *generation, snapshot, 0, 0));
}

TEST(AllesActorStore, NewAttachmentRetainsStoreAndOldLogoutCannotCloseIt)
{
    ActorStore store;
    auto const generation = Load(store, Listener, 10);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    store.RequestFlush(Listener);
    auto const save = store.CaptureSave(Listener, 1000);
    ASSERT_TRUE(save);
    EXPECT_EQ(store.Activate(Listener, 11), generation);
    store.Close(Listener, 10);
    EXPECT_EQ(store.Status(Listener)->state, ActorState::Ready);
    EXPECT_EQ(store.Status(Listener)->attachment, 11u);
    EXPECT_TRUE(FormFirst(store, Listener, generation));
    EXPECT_EQ(store.FindReady(Listener)->memories.size(), 1u);
    ASSERT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 3000));
    EXPECT_LT(store.Status(Listener)->committedRevision, store.Status(Listener)->revision);
    EXPECT_FALSE(store.EvictClosed(Listener));
    EXPECT_EQ(store.Activate(Listener, 0), generation);
    EXPECT_EQ(store.Status(Listener)->attachment, 11u);
}

TEST(AllesActorStore, OldLogoutQueuedBeforeNewAttachmentAndLoadCompletionRetainsInput)
{
    ActorStore store;
    auto const generation = store.Activate(Listener, 10);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    store.Close(Listener, 10);
    EXPECT_EQ(store.Activate(Listener, 11), generation);
    OwnerSnapshot snapshot;
    snapshot.owner = Listener;
    ASSERT_TRUE(store.FinishLoad(Listener, *generation, snapshot, 1000, 1000));
    EXPECT_EQ(store.Status(Listener)->state, ActorState::Ready);
    EXPECT_EQ(store.Status(Listener)->attachment, 11u);
    EXPECT_EQ(store.FindReady(Listener)->perceptions.size(), 1u);
}

TEST(AllesActorStore, LateSaveReceiptCannotAcknowledgeNewerStateOrAnotherGeneration)
{
    ActorStore store;
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    store.RequestFlush(Listener);
    auto const save = store.CaptureSave(Listener, 1000);
    ASSERT_TRUE(save);
    ASSERT_TRUE(store.Observe(Listener, Speech("a different line"), 2000));
    EXPECT_FALSE(store.CompleteSave(Listener, generation + 1, save->snapshot.revision, true, 3000));
    EXPECT_FALSE(store.CompleteSave(Listener, generation, save->snapshot.revision + 1, true, 3000));
    EXPECT_TRUE(store.Status(Listener)->saving);
    ASSERT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 3000));
    EXPECT_EQ(store.Status(Listener)->committedRevision, save->snapshot.revision);
    EXPECT_GT(store.Status(Listener)->revision, save->snapshot.revision);
    EXPECT_FALSE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 3000));
    EXPECT_FALSE(store.CaptureSave(Listener, 31999));
    EXPECT_TRUE(store.CaptureSave(Listener, 32000));
}

TEST(AllesActorStore, FailedSaveRetainsDirtyStateAndRetriesWithoutOvertaking)
{
    ActorStore store;
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    ASSERT_TRUE(FormFirst(store, Listener, generation));
    store.Close(Listener, 1);
    auto const save = store.CaptureSave(Listener, 2000);
    ASSERT_TRUE(save);
    EXPECT_FALSE(store.CaptureSave(Listener, 100000));
    EXPECT_FALSE(store.EvictClosed(Listener));
    ASSERT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, false, 3000));
    EXPECT_TRUE(store.Status(Listener)->saveFailed);
    EXPECT_EQ(store.Status(Listener)->committedRevision, 0u);
    EXPECT_FALSE(store.EvictClosed(Listener));
    EXPECT_FALSE(store.CaptureSave(Listener, 7999));
    auto const retry = store.CaptureSave(Listener, 8000);
    ASSERT_TRUE(retry);
    EXPECT_EQ(retry->snapshot.revision, save->snapshot.revision);
    ASSERT_TRUE(store.CompleteSave(Listener, generation, retry->snapshot.revision, true, 9000));
    EXPECT_TRUE(store.EvictClosed(Listener));
    auto const nextGeneration = store.Activate(Listener, 2);
    ASSERT_TRUE(nextGeneration);
    EXPECT_NE(*nextGeneration, generation);
    EXPECT_FALSE(store.FinishLoad(Listener, generation, retry->snapshot, 9000, 9000));
}

TEST(AllesActorStore, DrainedLogoutCanEvictAfterDecayTicksAndReloadAppliesElapsedDecay)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 10000;
    ActorStore store({}, policy);
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    ASSERT_TRUE(FormFirst(store, Listener, generation));
    store.Close(Listener, 1);
    auto const save = store.CaptureSave(Listener, 2000);
    ASSERT_TRUE(save);
    auto const salience = save->snapshot.memories.front().salience;

    store.Decay(3000, 3000);
    EXPECT_EQ(store.Status(Listener)->revision, save->snapshot.revision);
    ASSERT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 4000));
    store.Decay(4000, 4000);
    EXPECT_TRUE(store.EvictClosed(Listener));

    auto const reloaded = store.Activate(Listener, 2);
    ASSERT_TRUE(reloaded);
    EXPECT_NE(*reloaded, generation);
    ASSERT_TRUE(store.FinishLoad(Listener, *reloaded, save->snapshot, 12000, 12000));
    auto const* snapshot = store.FindReady(Listener);
    ASSERT_EQ(snapshot->memories.size(), 1u);
    EXPECT_DOUBLE_EQ(snapshot->memories.front().salience, salience / 2);
    EXPECT_GT(snapshot->revision, save->snapshot.revision);
}

TEST(AllesActorStore, OfflinePendingInterpretationStillDecaysItsMemoryContext)
{
    ActorStore store;
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    ASSERT_TRUE(FormFirst(store, Listener, generation));
    ASSERT_TRUE(store.Observe(Listener, Speech("another line", 3000), 3000));
    store.Close(Listener, 1);
    auto const revision = store.Status(Listener)->revision;
    auto const salience = store.FindReady(Listener)->memories.front().salience;

    store.Decay(4000, 4000);
    EXPECT_GT(store.Status(Listener)->revision, revision);
    EXPECT_LT(store.FindReady(Listener)->memories.front().salience, salience);
    EXPECT_EQ(store.FindReady(Listener)->perceptions.size(), 1u);
    EXPECT_FALSE(store.EvictClosed(Listener));
}

TEST(AllesActorStore, AdmissionAndGlobalSaveCapsAreBounded)
{
    ActorStore store({2, 2, 2, 1});
    auto const generation = Load(store);
    ActorKey const other{ActorKind::Player, 44};
    Load(store, other);
    EXPECT_FALSE(store.Activate({ActorKind::Player, 45}, 1));
    ASSERT_TRUE(store.Observe(Listener, Speech("first"), 1000));
    ASSERT_TRUE(store.Observe(Listener, Speech("second"), 1000));
    EXPECT_FALSE(store.Observe(Listener, Speech("overflow"), 1000));
    EXPECT_EQ(store.Status(Listener)->droppedPerceptions, 1u);
    EXPECT_EQ(store.FindReady(Listener)->perceptions.front().text, "first");
    ASSERT_TRUE(store.Observe(other, Speech(), 1000));
    store.RequestFlush(Listener);
    store.RequestFlush(other);
    auto const save = store.CaptureSave(Listener, 1000);
    ASSERT_TRUE(save);
    EXPECT_FALSE(store.CaptureSave(other, 1000));
    EXPECT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 2000));
    EXPECT_TRUE(store.CaptureSave(other, 2000));
}

TEST(AllesActorStore, FinalCaptureBypassesRetryDelayOnlyAfterAnOlderWriteIsTerminal)
{
    ActorStore store;
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    store.RequestFlush(Listener);
    auto const older = store.CaptureSave(Listener, 1000);
    ASSERT_TRUE(older);
    ASSERT_TRUE(FormFirst(store, Listener, generation));
    EXPECT_FALSE(store.CaptureFinalSave(Listener, 2000));
    ASSERT_TRUE(store.CompleteSave(Listener, generation, older->snapshot.revision, false, 3000));
    EXPECT_FALSE(store.CaptureSave(Listener, 3000));
    auto const final = store.CaptureFinalSave(Listener, 3000);
    ASSERT_TRUE(final);
    EXPECT_GT(final->snapshot.revision, older->snapshot.revision);
    EXPECT_EQ(final->snapshot.memories.size(), 1u);
    EXPECT_TRUE(final->snapshot.perceptions.empty());
    ASSERT_TRUE(store.CompleteSave(Listener, generation, final->snapshot.revision, true, 3000));
    EXPECT_EQ(store.Status(Listener)->committedRevision, final->snapshot.revision);
}

TEST(AllesActorStore, LoadingOverflowIsCountedAndNeverEvictsPersistedInputs)
{
    ActorStore store({1, 2, 1, 1});
    auto const generation = store.Activate(Listener, 1);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(store.Observe(Listener, Speech("live ingress"), 1000));
    OwnerSnapshot snapshot;
    snapshot.owner = Listener;
    auto persisted = Speech("already admitted");
    persisted.id = 1;
    snapshot.nextPerceptionId = 2;
    snapshot.perceptions.push_back(persisted);
    ASSERT_TRUE(store.FinishLoad(Listener, *generation, snapshot, 1000, 1000));
    EXPECT_EQ(store.FindReady(Listener)->perceptions.front().text, "already admitted");
    EXPECT_EQ(store.Status(Listener)->droppedPerceptions, 1u);
    ASSERT_TRUE(FormFirst(store, Listener, *generation));
    EXPECT_TRUE(store.Observe(Listener, Speech("live ingress"), 2000));
}

TEST(AllesActorStore, SaturatedSnapshotSchedulingRotatesFairlyAcrossOwners)
{
    ActorStore store({3, 2, 4, 1});
    ActorKey const second{ActorKind::Player, 44};
    ActorKey const third{ActorKind::Player, 45};
    Load(store);
    Load(store, second);
    Load(store, third);
    for (auto const owner : {Listener, second, third})
    {
        ASSERT_TRUE(store.Observe(owner, Speech(), 1000));
        store.RequestFlush(owner);
    }
    EXPECT_TRUE(store.CaptureDueSaves(1000, 0).empty());
    for (auto const expected : {Listener, second, third})
    {
        auto const requests = store.CaptureDueSaves(1000);
        ASSERT_EQ(requests.size(), 1u);
        auto const& request = requests.front();
        EXPECT_EQ(request.snapshot.owner, expected);
        EXPECT_TRUE(store.CaptureDueSaves(1000).empty());
        ASSERT_TRUE(store.CompleteSave(expected, request.generation, request.snapshot.revision, true, 1000));
        ASSERT_TRUE(store.Observe(expected, Speech("new changes"), 1000));
        store.RequestFlush(expected);
    }
}

TEST(AllesActorStore, RepeatedUtterancesHaveAFixedWindowAndIndependentEmissionDeduplication)
{
    ActorStore store;
    Load(store);
    auto line = Speech("ambient", 1000);
    line.emissionId = 1;
    ASSERT_TRUE(store.Observe(Listener, line, 1000));
    for (uint64_t emission = 2; emission <= 100; ++emission)
    {
        line.emissionId = emission;
        line.gameTimeMs = 2000 + emission;
        EXPECT_FALSE(store.Observe(Listener, line, 2000 + emission));
    }
    line.text = "another locale of the suppressed repeat";
    EXPECT_FALSE(store.Observe(Listener, line, 3000));
    line.emissionId = 101;
    EXPECT_TRUE(store.Observe(Listener, line, 3000));
    line.text = "ambient";
    line.emissionId = 102;
    line.source.name = "Localized speaker name";
    EXPECT_FALSE(store.Observe(Listener, line, 3000));
    line.emissionId = 104;
    line.gameTimeMs = 31000;
    EXPECT_TRUE(store.Observe(Listener, line, 31000));
    line.emissionId = 103;
    line.source.actor = ActorKey{ActorKind::CreatureSpawn, Speaker.id};
    EXPECT_TRUE(store.Observe(Listener, line, 31000));
}

TEST(AllesActorStore, SelfRetellingAndGatedForeignRepeatsCannotCorroborate)
{
    ActorStore store;
    Load(store);
    auto line = Speech();
    line.source.actor = Listener;
    EXPECT_FALSE(store.Observe(Listener, line, 1000));
    line.source.actor = Speaker;
    line.comprehended = false;
    EXPECT_TRUE(store.Observe(Listener, line, 1000));
    line.text = "different plaintext in the same unknown language";
    EXPECT_FALSE(store.Observe(Listener, line, 2000));
    EXPECT_TRUE(store.FindReady(Listener)->perceptions.front().text.empty());
}

TEST(AllesActorStore, AcceptanceRejectsOutOfOrderInputsAndStaleMemoryRevisionsAtomically)
{
    ActorStore store;
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech("first"), 1000));
    ASSERT_TRUE(store.Observe(Listener, Speech("second"), 1000));
    auto const* snapshot = store.FindReady(Listener);
    auto const first = snapshot->perceptions[0];
    auto const second = snapshot->perceptions[1];
    auto memory = FormFallback(first, {}, 2000);
    EXPECT_FALSE(store.Apply(Listener, generation, {second.id}, {}, {memory}, 2000, 2000));
    EXPECT_FALSE(store.Apply(Listener, generation + 1, {first.id}, {}, {memory}, 2000, 2000));
    EXPECT_TRUE(snapshot->memories.empty());
    ASSERT_TRUE(store.Apply(Listener, generation, {first.id}, {}, {memory}, 2000, 2000));
    auto const memoryId = snapshot->memories.front().id;
    auto const revision = snapshot->revision;
    EXPECT_FALSE(store.Apply(Listener, generation, {second.id}, {{memoryId, 2}}, {memory}, 2000, 2000));
    EXPECT_EQ(snapshot->revision, revision);
    EXPECT_EQ(snapshot->perceptions.size(), 1u);
    memory.confidence = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(store.Apply(Listener, generation, {second.id}, {}, {memory}, 2000, 2000));
    EXPECT_EQ(snapshot->revision, revision);
    memory.confidence = 0.9;
    EXPECT_FALSE(store.Apply(Listener, generation, {second.id}, {}, {memory}, 2000, 2000));
}

TEST(AllesActorStore, ForgottenMemoriesCannotBeUsedByAStaleProposal)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 1000;
    ActorStore store({}, policy);
    auto const generation = Load(store);
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    ASSERT_TRUE(FormFirst(store, Listener, generation));
    auto const memory = store.FindReady(Listener)->memories.front();
    ASSERT_TRUE(store.Observe(Listener, Speech("later input"), 3000));
    auto const input = store.FindReady(Listener)->perceptions.front();
    store.Decay(20000, 20000);
    ASSERT_TRUE(store.FindReady(Listener)->memories.empty());
    EXPECT_FALSE(store.Apply(Listener, generation, {input.id}, {{memory.id, memory.contentRevision}},
        {FormFallback(input, policy, 20000)}, 20000, 20000));
    EXPECT_EQ(store.FindReady(Listener)->perceptions.size(), 1u);
}

TEST(AllesActorStore, OfflineDecayUsesPersistedCheckpointOnceAndLoginDoesNotForceCleanWrites)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 1000;
    ActorStore store({}, policy);
    auto const generation = store.Activate(Listener, 1);
    ASSERT_TRUE(generation);
    OwnerSnapshot snapshot;
    snapshot.owner = Listener;
    snapshot.revision = 12;
    snapshot.nextMemoryId = 2;
    snapshot.decayGameTimeMs = 1000;
    auto memory = FormFallback(Speech(), policy, 1000);
    memory.id = 1;
    memory.salience = 1;
    snapshot.memories.push_back(memory);
    ASSERT_TRUE(store.FinishLoad(Listener, *generation, snapshot, 2000, 5000));
    EXPECT_DOUBLE_EQ(store.FindReady(Listener)->memories.front().salience, 0.5);
    auto const revision = store.Status(Listener)->revision;
    store.Decay(2000, 6000);
    EXPECT_DOUBLE_EQ(store.FindReady(Listener)->memories.front().salience, 0.5);
    EXPECT_EQ(store.Status(Listener)->revision, revision);
    EXPECT_FALSE(store.CaptureSave(Listener, 34999));
    EXPECT_TRUE(store.CaptureSave(Listener, 35000));
}

TEST(AllesActorStore, CleanAndAlreadyCapturedFlushRequestsDoNotForceFutureChangesToFlush)
{
    ActorStore store;
    auto const generation = Load(store);
    store.RequestFlush(Listener);
    EXPECT_FALSE(store.CaptureSave(Listener, 0));
    ASSERT_TRUE(store.Observe(Listener, Speech(), 1000));
    EXPECT_FALSE(store.CaptureSave(Listener, 1000));
    store.RequestFlush(Listener);
    auto const save = store.CaptureSave(Listener, 1000);
    ASSERT_TRUE(save);
    store.RequestFlush(Listener);
    ASSERT_TRUE(store.Observe(Listener, Speech("newer"), 2000));
    ASSERT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 3000));
    EXPECT_FALSE(store.CaptureSave(Listener, 3000));
    EXPECT_TRUE(store.CaptureSave(Listener, 32000));
}

TEST(AllesActorStore, MaterialThresholdHasAFiveSecondMinimumAndDecayDoesNotCountEveryTick)
{
    ActorStore store;
    auto const generation = Load(store);
    for (unsigned index = 0; index < 64; ++index)
        ASSERT_TRUE(store.Observe(Listener, Speech("line " + std::to_string(index)), 0));
    EXPECT_FALSE(store.CaptureSave(Listener, 4999));
    auto const save = store.CaptureSave(Listener, 5000);
    ASSERT_TRUE(save);
    ASSERT_TRUE(store.CompleteSave(Listener, generation, save->snapshot.revision, true, 5000));

    ASSERT_TRUE(FormFirst(store, Listener, generation, 6000));
    for (uint64_t now = 6001; now < 6200; ++now)
        store.Decay(now, now);
    EXPECT_FALSE(store.CaptureSave(Listener, 12000));
    EXPECT_TRUE(store.CaptureSave(Listener, 36000));
}
}
