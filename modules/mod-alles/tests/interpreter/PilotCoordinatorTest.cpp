/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "interpreter/PilotCoordinator.h"
#include "gtest/gtest.h"
#include <limits>

namespace Alles::Interpreter
{
namespace
{
constexpr ActorKey Player{ActorKind::Player, 1};
constexpr ActorKey Other{ActorKind::CreatureSpawn, 1};

struct Village
{
    ActorStore store;
    PilotCoordinator coordinator{store};

    uint64_t Add(ActorKey owner, OwnerSnapshot snapshot = {})
    {
        auto generation = store.Activate(owner, 1);
        if (!generation)
            return 0;
        snapshot.owner = owner;
        if (!store.FinishLoad(owner, *generation, std::move(snapshot), 0, 0) || !coordinator.Track(owner))
            return 0;
        return *generation;
    }

    bool Hear(ActorKey owner, std::string text, uint64_t gameMs = 0, uint64_t realMs = 0, uint64_t speaker = 99)
    {
        Perception perception;
        perception.source = {ActorKey{ActorKind::Player, speaker}, "Speaker"};
        perception.text = std::move(text);
        perception.place = "Northshire";
        perception.selfContext = "I am a village resident.";
        perception.gameTimeMs = gameMs;
        return store.Observe(owner, std::move(perception), realMs);
    }
};

TEST(AllesPilotTest, ExternalRequiresPermitAndFencesReplay)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.EnableExternal();
    ASSERT_TRUE(village.Hear(Player, "a wolf is wounded"));
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Claim("worker", "profile", 5000, 5000);
    ASSERT_TRUE(job);
    EXPECT_TRUE(job->permitId.empty());
    EXPECT_FALSE(village.coordinator.Claim("other", "profile", 5000, 5000));
    auto proposal = MakeFakeProposal(*job, {});
    EXPECT_EQ(village.coordinator.ApplyExternal(proposal, 5000, 5000), "stale");
    EXPECT_FALSE(village.coordinator.Authorize(job->jobToken, "other", job->leaseGeneration, "permit", 5000, 5000));
    ASSERT_TRUE(village.coordinator.Authorize(job->jobToken, "worker", job->leaseGeneration, "permit", 5000, 5000));
    EXPECT_FALSE(village.coordinator.Authorize(job->jobToken, "worker", job->leaseGeneration, "second", 5000, 5000));
    EXPECT_FALSE(village.coordinator.Release(job->jobToken, "worker", job->leaseGeneration));
    proposal.permitId = "permit";
    EXPECT_EQ(village.coordinator.ApplyExternal(proposal, 5001, 5001), "applied");
    EXPECT_EQ(village.coordinator.ApplyExternal(proposal, 5002, 5002), "stale");
    auto const* memory = village.store.FindReady(Player);
    ASSERT_EQ(memory->memories.size(), 1u);
    EXPECT_EQ(memory->memories.front().formation, FormationMode::Model);
    EXPECT_EQ(village.coordinator.Stats().modelMemories, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
}

TEST(AllesPilotTest, RoutineDeathsUseReflexWithoutConsumingModelWorkButPvpRetainsInterpretation)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.EnableExternal();
    Perception death;
    death.kind = PerceptionKind::WitnessedDeath;
    death.subject = {ActorKey{ActorKind::CreatureSpawn, 20}, "Wolf"};
    death.source = {ActorKey{ActorKind::Player, 19}, "Hunter"};
    ASSERT_TRUE(village.store.Observe(Player, death, 0));
    village.coordinator.Update(0, 0);
    auto const* snapshot = village.store.FindReady(Player);
    ASSERT_EQ(snapshot->memories.size(), 1u);
    EXPECT_EQ(snapshot->memories[0].formation, FormationMode::Reflex);
    EXPECT_DOUBLE_EQ(snapshot->memories[0].salience, 0.05);
    EXPECT_EQ(village.coordinator.Stats().reflexMemories, 1u);
    EXPECT_FALSE(village.coordinator.Claim("worker", "profile", 5000, 5000));

    death.subject = {ActorKey{ActorKind::Player, 20}, "Victim"};
    death.gameTimeMs = 5000;
    ASSERT_TRUE(village.store.Observe(Player, death, 5000));
    village.coordinator.Update(10000, 10000);
    auto const job = village.coordinator.Claim("worker", "profile", 10000, 10000);
    ASSERT_TRUE(job);
    ASSERT_EQ(job->perceptions.size(), 1u);
    EXPECT_EQ(job->perceptions[0].value.subject, death.subject);
}

TEST(AllesPilotTest, ExternalLostWorkerExpiresAndCannotRenew)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.EnableExternal();
    ASSERT_TRUE(village.Hear(Player, "news"));
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Claim("worker", "profile", 5000, 5000);
    ASSERT_TRUE(job);
    ASSERT_TRUE(village.coordinator.Authorize(job->jobToken, "worker", job->leaseGeneration, "permit", 5000, 5000));
    EXPECT_FALSE(village.coordinator.Heartbeat(job->jobToken, "other", job->leaseGeneration, 10000));
    village.coordinator.Update(20000, 20000);
    EXPECT_FALSE(village.coordinator.Heartbeat(job->jobToken, "worker", job->leaseGeneration, 20000));
    EXPECT_EQ(village.store.FindReady(Player)->memories.front().formation, FormationMode::Fallback);
}

TEST(AllesPilotTest, ExternalReleaseKeepsAdmissionAndRejectsOldLease)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.EnableExternal();
    ASSERT_TRUE(village.Hear(Player, "news"));
    village.coordinator.Update(5000, 5000);
    auto first = village.coordinator.Claim("worker", "profile", 5000, 5000);
    ASSERT_TRUE(first);
    ASSERT_TRUE(village.coordinator.Release(first->jobToken, "worker", first->leaseGeneration));
    auto second = village.coordinator.Claim("other", "profile", 20000, 20000);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->admittedRealTimeMs, second->admittedRealTimeMs);
    EXPECT_GT(second->leaseGeneration, first->leaseGeneration);
    EXPECT_FALSE(village.coordinator.Authorize(first->jobToken, "worker", first->leaseGeneration, "old", 20000, 20000));
    EXPECT_FALSE(village.coordinator.Authorize(second->jobToken, "other", second->leaseGeneration, "late", 20001, 20001));
}

TEST(AllesPilotTest, DebounceThenQueuedFakeUsesAllEightInputs)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    for (unsigned index = 0; index < 8; ++index)
        ASSERT_TRUE(village.Hear(Player, "claim " + std::to_string(index)));
    village.coordinator.Update(4999, 4999);
    EXPECT_FALSE(village.coordinator.Inspect(Player));
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Inspect(Player);
    ASSERT_TRUE(job);
    ASSERT_EQ(job->perceptions.size(), 8u);
    EXPECT_LE(job->memories.size(), 12u);
    EXPECT_LE(job->contextBytesBound, 24u * 1024);
    EXPECT_EQ(job->httpAttemptCount, 0u);
    EXPECT_EQ(job->jobToken.size(), 32u);
    EXPECT_EQ(job->bootEpoch.size(), 32u);
    EXPECT_TRUE(job->permitId.starts_with("fake:"));
    EXPECT_TRUE(village.store.FindReady(Player)->memories.empty());
    village.coordinator.Update(5001, 5001);
    auto const* snapshot = village.store.FindReady(Player);
    ASSERT_TRUE(snapshot->perceptions.empty());
    ASSERT_EQ(snapshot->memories.size(), 1u);
    for (unsigned index = 0; index < 8; ++index)
        EXPECT_NE(snapshot->memories.front().claim.find("claim " + std::to_string(index)), std::string::npos);
    EXPECT_EQ(snapshot->memories.front().formation, FormationMode::InProcessFake);
}

TEST(AllesPilotTest, IncompatibleSourcesCloseOnlyThatBatchSooner)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    for (unsigned index = 0; index < 8; ++index)
        ASSERT_TRUE(village.Hear(Player, "news " + std::to_string(index), 0, 0, 100 + index));
    village.coordinator.Update(5000, 5000);
    ASSERT_EQ(village.coordinator.Inspect(Player)->perceptions.size(), 4u);
    village.coordinator.Update(5001, 5001);
    EXPECT_EQ(village.store.FindReady(Player)->perceptions.size(), 4u);
    EXPECT_EQ(village.store.FindReady(Player)->memories.size(), 4u);
    village.coordinator.Update(19999, 19999);
    EXPECT_EQ(village.coordinator.Stats().dispatched, 1u);
    village.coordinator.Update(20000, 20000);
    village.coordinator.Update(20001, 20001);
    EXPECT_TRUE(village.store.FindReady(Player)->perceptions.empty());
    EXPECT_EQ(village.store.FindReady(Player)->memories.size(), 8u);
}

TEST(AllesPilotTest, LongClaimsRemainCompleteWithinFourOutputs)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    for (unsigned index = 0; index < 8; ++index)
        ASSERT_TRUE(village.Hear(Player, std::string(300, char('a' + index))));
    village.coordinator.Update(5000, 5000);
    ASSERT_EQ(village.coordinator.Inspect(Player)->perceptions.size(), 4u);
    village.coordinator.Update(5001, 5001);
    auto const* snapshot = village.store.FindReady(Player);
    ASSERT_EQ(snapshot->memories.size(), 4u);
    EXPECT_EQ(snapshot->perceptions.size(), 4u);
    for (std::size_t index = 0; index < 4; ++index)
        EXPECT_EQ(snapshot->memories[index].claim, std::string(300, char('a' + index)));
}

TEST(AllesPilotTest, DelayedFakeSnapshotDoesNotAbsorbNewInputs)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.SetFakeBehavior({FakeMode::Valid, 2000, true});
    ASSERT_TRUE(village.Hear(Player, "first"));
    village.coordinator.Update(5000, 5000);
    auto const original = village.coordinator.Inspect(Player);
    ASSERT_TRUE(village.Hear(Player, "later", 6000, 6000));
    village.coordinator.Update(6000, 6000);
    ASSERT_EQ(village.coordinator.Inspect(Player)->perceptions.size(), 1u);
    EXPECT_EQ(village.coordinator.Inspect(Player)->jobToken, original->jobToken);
    village.coordinator.Update(7000, 7000);
    EXPECT_TRUE(village.store.FindReady(Player)->memories.empty());
    village.coordinator.Update(7001, 7001);
    ASSERT_EQ(village.store.FindReady(Player)->memories.size(), 1u);
    ASSERT_EQ(village.store.FindReady(Player)->perceptions.size(), 1u);
    EXPECT_EQ(village.store.FindReady(Player)->perceptions.front().text, "later");
}

TEST(AllesPilotTest, WrongPermitWorkerEpochAndDuplicateNeverMutateStore)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_TRUE(village.Hear(Player, "one rumor"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto const job = *village.coordinator.Inspect(Player);
    auto valid = MakeFakeProposal(job, {});
    auto wrongPermit = valid;
    wrongPermit.permitId = "fake:wrong";
    auto wrongWorker = valid;
    wrongWorker.workerId = "another-worker";
    auto wrongEpoch = valid;
    wrongEpoch.bootEpoch = "previous-boot";
    auto wrongGeneration = valid;
    ++wrongGeneration.actorGeneration;
    auto wrongLease = valid;
    ++wrongLease.leaseGeneration;
    ASSERT_TRUE(village.coordinator.Submit(wrongPermit));
    ASSERT_TRUE(village.coordinator.Submit(wrongWorker));
    ASSERT_TRUE(village.coordinator.Submit(wrongEpoch));
    ASSERT_TRUE(village.coordinator.Submit(wrongGeneration));
    ASSERT_TRUE(village.coordinator.Submit(wrongLease));
    auto const revision = village.store.Status(Player)->revision;
    village.coordinator.Update(5001, 5001);
    EXPECT_EQ(village.store.Status(Player)->revision, revision);
    EXPECT_EQ(village.coordinator.Stats().staleResults, 5u);
    ASSERT_TRUE(village.coordinator.Submit(valid));
    village.coordinator.Update(5002, 5002);
    auto const acceptedRevision = village.store.Status(Player)->revision;
    ASSERT_TRUE(village.coordinator.Submit(valid));
    village.coordinator.Update(5003, 5003);
    EXPECT_EQ(village.store.Status(Player)->revision, acceptedRevision);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 1u);
}

TEST(AllesPilotTest, MissingSupportRejectsAtomicallyThenFallbackRetainsEveryClaim)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    for (unsigned index = 0; index < 8; ++index)
        ASSERT_TRUE(village.Hear(Player, "fact " + std::to_string(index)));
    village.coordinator.Update(5000, 5000);
    auto result = MakeFakeProposal(*village.coordinator.Inspect(Player), {});
    ASSERT_EQ(result.memories.size(), 1u);
    result.memories.front().supportingPerceptions.pop_back();
    ASSERT_TRUE(village.coordinator.Submit(std::move(result)));
    village.coordinator.Update(5001, 5001);
    EXPECT_EQ(village.coordinator.Stats().invalidResults, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    EXPECT_TRUE(village.store.FindReady(Player)->perceptions.empty());
    ASSERT_EQ(village.store.FindReady(Player)->memories.size(), 1u);
    auto const& memory = village.store.FindReady(Player)->memories.front();
    EXPECT_EQ(memory.formation, FormationMode::Fallback);
    for (unsigned index = 0; index < 8; ++index)
        EXPECT_NE(memory.claim.find("fact " + std::to_string(index)), std::string::npos);
}

TEST(AllesPilotTest, CrossSourceGroupingAndNonFiniteScoresAreRejected)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    ASSERT_TRUE(village.Hear(Player, "first account", 0, 0, 99));
    ASSERT_TRUE(village.Hear(Player, "second account", 0, 0, 100));
    village.coordinator.Update(5000, 5000);
    auto result = MakeFakeProposal(*village.coordinator.Inspect(Player), {});
    ASSERT_EQ(result.memories.size(), 2u);
    result.memories.front().supportingPerceptions.push_back(result.memories.back().supportingPerceptions.front());
    result.memories.pop_back();
    ASSERT_TRUE(village.coordinator.Submit(result));
    village.coordinator.Update(5001, 5001);
    EXPECT_EQ(village.coordinator.Stats().invalidResults, 1u);
    ASSERT_EQ(village.store.FindReady(Player)->memories.size(), 2u);
    EXPECT_NE(village.store.FindReady(Player)->memories[0].source.actor,
        village.store.FindReady(Player)->memories[1].source.actor);

    Village second;
    ASSERT_NE(second.Add(Player), 0u);
    second.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    ASSERT_TRUE(second.Hear(Player, "another report"));
    second.coordinator.Update(5000, 5000);
    result = MakeFakeProposal(*second.coordinator.Inspect(Player), {});
    result.memories.front().confidence = std::numeric_limits<double>::quiet_NaN();
    ASSERT_TRUE(second.coordinator.Submit(result));
    second.coordinator.Update(5001, 5001);
    EXPECT_EQ(second.coordinator.Stats().invalidResults, 1u);
    EXPECT_EQ(second.coordinator.Stats().fakeMemories, 0u);
}

TEST(AllesPilotTest, ProcessingAfterLeaseExpiryCannotApplyQueuedValidResult)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_TRUE(village.Hear(Player, "pending"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    ASSERT_TRUE(village.coordinator.Submit(MakeFakeProposal(*village.coordinator.Inspect(Player), {})));
    village.coordinator.Update(20000, 20000);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    EXPECT_EQ(village.coordinator.Stats().fallbackMemories, 1u);
    EXPECT_EQ(village.coordinator.Stats().staleResults, 1u);
}

TEST(AllesPilotTest, AdmissionDeadlinesIncludeTimeBeforeAnyJobExists)
{
    Village real;
    ASSERT_NE(real.Add(Player), 0u);
    ASSERT_TRUE(real.Hear(Player, "while game time stands still"));
    real.coordinator.Update(0, 44999);
    EXPECT_TRUE(real.store.FindReady(Player)->memories.empty());
    real.coordinator.Update(0, 45000);
    EXPECT_EQ(real.coordinator.Stats().fallbackMemories, 1u);
    EXPECT_EQ(real.coordinator.Stats().dispatched, 0u);

    Village game;
    ASSERT_NE(game.Add(Player), 0u);
    ASSERT_TRUE(game.Hear(Player, "time moved quickly"));
    game.coordinator.Update(120000, 1000);
    EXPECT_EQ(game.coordinator.Stats().fallbackMemories, 1u);
    EXPECT_EQ(game.coordinator.Stats().dispatched, 0u);
}

TEST(AllesPilotTest, MetWaitsBehindAnEarlierJobThenUsesOrderedReflex)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_TRUE(village.Hear(Player, "before meeting"));
    Perception met;
    met.kind = PerceptionKind::Met;
    met.subject = {ActorKey{ActorKind::Player, 42}, "Visitor"};
    met.gameTimeMs = 1000;
    ASSERT_TRUE(village.store.Observe(Player, met, 1000));
    village.coordinator.SetFakeBehavior({FakeMode::Valid, 1000, true});
    village.coordinator.Update(5000, 5000);
    EXPECT_EQ(village.coordinator.Stats().reflexMemories, 0u);
    village.coordinator.Update(6000, 6000);
    village.coordinator.Update(6001, 6001);
    auto const* snapshot = village.store.FindReady(Player);
    ASSERT_EQ(snapshot->memories.size(), 2u);
    EXPECT_EQ(snapshot->memories[0].formation, FormationMode::InProcessFake);
    EXPECT_EQ(snapshot->memories[1].formation, FormationMode::Reflex);
    EXPECT_TRUE(snapshot->perceptions.empty());
}

TEST(AllesPilotTest, EqualTypedIdsHaveSeparateJobsContextsAndFifoDispatch)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_NE(village.Add(Other), 0u);
    ASSERT_TRUE(village.Hear(Other, "earlier", 0, 0));
    ASSERT_TRUE(village.Hear(Player, "later", 1, 1));
    village.coordinator.Update(5001, 5001);
    auto first = village.coordinator.Inspect(Other);
    auto second = village.coordinator.Inspect(Player);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(first->permitId.empty());
    EXPECT_TRUE(second->permitId.empty());
    EXPECT_NE(first->jobToken, second->jobToken);
    EXPECT_EQ(first->perceptions.front().value.text, "earlier");
    EXPECT_EQ(second->perceptions.front().value.text, "later");
    village.coordinator.Update(5002, 5002);
    village.coordinator.Update(5003, 5003);
    EXPECT_EQ(village.store.FindReady(Other)->memories.front().claim, "earlier");
    EXPECT_EQ(village.store.FindReady(Player)->memories.front().claim, "later");
}

TEST(AllesPilotTest, ShutdownFencesFakeAndDrainsWithoutLosingInputs)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    for (unsigned index = 0; index < 12; ++index)
        ASSERT_TRUE(village.Hear(Player, "retained " + std::to_string(index)));
    village.coordinator.Update(5000, 5000);
    auto result = MakeFakeProposal(*village.coordinator.Inspect(Player), {});
    village.coordinator.Stop();
    EXPECT_FALSE(village.coordinator.Submit(result));
    while (village.coordinator.DrainFallback(5001, 5001, 1)) { }
    EXPECT_TRUE(village.store.FindReady(Player)->perceptions.empty());
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    ASSERT_EQ(village.store.FindReady(Player)->memories.size(), 2u);
}
TEST(AllesPilotTest, ForgottenMemoryContextFencesResultWithoutConsumingInputs)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 100;
    ActorStore store({}, policy);
    PilotCoordinator coordinator(store, policy);
    auto generation = store.Activate(Player, 1);
    ASSERT_TRUE(generation);
    OwnerSnapshot snapshot;
    snapshot.owner = Player;
    snapshot.revision = 1;
    snapshot.nextMemoryId = 2;
    Memory memory;
    memory.id = 1;
    memory.claim = "an earlier report";
    memory.confidence = 0.5;
    memory.salience = 0.7;
    snapshot.memories.push_back(memory);
    ASSERT_TRUE(store.FinishLoad(Player, *generation, snapshot, 0, 0));
    ASSERT_TRUE(coordinator.Track(Player));
    Perception perception;
    perception.text = "new observation";
    ASSERT_TRUE(store.Observe(Player, perception, 0));
    coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    coordinator.Update(5000, 5000);
    auto job = coordinator.Inspect(Player);
    ASSERT_TRUE(job);
    ASSERT_EQ(job->memories.size(), 1u);
    store.Decay(5001, 5001);
    ASSERT_TRUE(store.FindReady(Player)->memories.empty());
    auto const revision = store.Status(Player)->revision;
    ASSERT_TRUE(coordinator.Submit(MakeFakeProposal(*job, policy)));
    coordinator.Update(5002, 5002);
    EXPECT_EQ(store.Status(Player)->revision, revision);
    EXPECT_EQ(store.FindReady(Player)->perceptions.size(), 1u);
    EXPECT_EQ(coordinator.Stats().staleResults, 1u);
}

TEST(AllesPilotTest, SalienceOnlyRehearsalDoesNotReplaceCurrentMemoryValues)
{
    Village village;
    OwnerSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.nextMemoryId = 2;
    Memory memory;
    memory.id = 1;
    memory.claim = "already remembered";
    memory.confidence = 0.5;
    memory.salience = 0.3;
    snapshot.memories.push_back(memory);
    ASSERT_NE(village.Add(Player, snapshot), 0u);
    ASSERT_TRUE(village.Hear(Player, "new report"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto const job = *village.coordinator.Inspect(Player);
    ASSERT_TRUE(village.store.Rehearse(Player, 1, 5001, 5001, 0.2));
    auto const salience = village.store.FindReady(Player)->memories.front().salience;
    ASSERT_TRUE(village.coordinator.Submit(MakeFakeProposal(job, {})));
    village.coordinator.Update(5002, 5002);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 1u);
    EXPECT_DOUBLE_EQ(village.store.FindReady(Player)->memories.front().salience, salience);
}

TEST(AllesPilotTest, WithheldFakePermitExpiresDespiteLeaseHeartbeats)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_TRUE(village.Hear(Player, "unfinished interpretation"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, true});
    village.coordinator.Update(5000, 5000);
    auto const permitExpiry = village.coordinator.Inspect(Player)->permitExpiresRealTimeMs;
    for (uint64_t now = 10000; now < permitExpiry; now += 5000)
        village.coordinator.Update(now, now);
    ASSERT_TRUE(village.coordinator.Inspect(Player));
    EXPECT_GT(village.coordinator.Inspect(Player)->leaseExpiresRealTimeMs, permitExpiry);
    village.coordinator.Update(permitExpiry, permitExpiry);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    EXPECT_EQ(village.coordinator.Stats().fallbackMemories, 1u);
}

TEST(AllesPilotTest, MalformedAndStaleFakeFixturesShareTheAcceptancePath)
{
    Village malformed;
    ASSERT_NE(malformed.Add(Player), 0u);
    ASSERT_TRUE(malformed.Hear(Player, "heard report"));
    malformed.coordinator.SetFakeBehavior({FakeMode::Malformed, 0, false});
    malformed.coordinator.Update(5000, 5000);
    malformed.coordinator.Update(5001, 5001);
    EXPECT_EQ(malformed.coordinator.Stats().invalidResults, 1u);
    EXPECT_EQ(malformed.coordinator.Stats().fallbackMemories, 1u);

    Village stale;
    ASSERT_NE(stale.Add(Player), 0u);
    ASSERT_TRUE(stale.Hear(Player, "still pending"));
    stale.coordinator.SetFakeBehavior({FakeMode::Stale, 0, false});
    stale.coordinator.Update(5000, 5000);
    stale.coordinator.Update(5001, 5001);
    EXPECT_EQ(stale.coordinator.Stats().staleResults, 1u);
    EXPECT_TRUE(stale.store.FindReady(Player)->memories.empty());
    EXPECT_EQ(stale.store.FindReady(Player)->perceptions.size(), 1u);
}

TEST(AllesPilotTest, UnknownTargetFencesResultWithoutConsumingInputs)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_TRUE(village.Hear(Player, "heard report"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto result = MakeFakeProposal(*village.coordinator.Inspect(Player), {});
    result.memories.front().operation = ProposalOperation::Reinforce;
    result.memories.front().targetMemoryToken = "m1";
    ASSERT_TRUE(village.coordinator.Submit(result));
    village.coordinator.Update(5001, 5001);
    EXPECT_EQ(village.coordinator.Stats().invalidResults, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    EXPECT_EQ(village.coordinator.Stats().fallbackMemories, 0u);
    EXPECT_TRUE(village.store.FindReady(Player)->memories.empty());
    EXPECT_EQ(village.store.FindReady(Player)->perceptions.size(), 1u);
}

OwnerSnapshot ExistingBelief()
{
    OwnerSnapshot snapshot;
    Memory memory;
    memory.id = 1;
    memory.claim = "an existing belief";
    memory.source = {ActorKey{ActorKind::Player, 50}, "Original source"};
    memory.attribution = "I heard";
    memory.confidence = 0.2;
    memory.salience = 0.5;
    snapshot.memories = {memory};
    snapshot.nextMemoryId = 2;
    return snapshot;
}

TEST(AllesPilotTest, ReinforceUsesCurrentLocalNumbersAndRetainsOriginalProvenance)
{
    Village village;
    auto initial = ExistingBelief();
    ASSERT_NE(village.Add(Player, initial), 0u);
    ASSERT_TRUE(village.Hear(Player, "an existing belief"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Inspect(Player);
    ASSERT_TRUE(job);
    ASSERT_EQ(job->memories.size(), 1u);
    auto result = MakeFakeProposal(*job, {});
    result.memories[0].operation = ProposalOperation::Reinforce;
    result.memories[0].targetMemoryToken = job->memories[0].token;
    result.memories[0].confidence = 1;
    result.memories[0].salience = 0;
    ASSERT_TRUE(village.store.Rehearse(Player, 1, 5000, 5000, 0.2));
    auto const current = village.store.FindReady(Player)->memories[0];
    ASSERT_TRUE(village.coordinator.Submit(result));
    village.coordinator.Update(5000, 5001);
    auto const* after = village.store.FindReady(Player);
    EXPECT_TRUE(after->perceptions.empty());
    ASSERT_EQ(after->memories.size(), 1u);
    EXPECT_EQ(after->memories[0].contentRevision, 1u);
    EXPECT_EQ(after->memories[0].source, initial.memories[0].source);
    EXPECT_EQ(after->memories[0].attribution, initial.memories[0].attribution);
    EXPECT_DOUBLE_EQ(after->memories[0].confidence, 0.2);
    EXPECT_DOUBLE_EQ(after->memories[0].salience, current.salience + 0.1);
    EXPECT_EQ(village.coordinator.Stats().fakeReinforcements, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
}

TEST(AllesPilotTest, MixedCreateAndSupersedeCommitTogetherUsingSupportedSource)
{
    Village village;
    ASSERT_NE(village.Add(Player, ExistingBelief()), 0u);
    ASSERT_TRUE(village.Hear(Player, "a correction", 0, 0, 99));
    ASSERT_TRUE(village.Hear(Player, "another report", 0, 0, 100));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Inspect(Player);
    ASSERT_TRUE(job);
    auto result = MakeFakeProposal(*job, {});
    ASSERT_EQ(result.memories.size(), 2u);
    result.memories[0].operation = ProposalOperation::Supersede;
    result.memories[0].targetMemoryToken = job->memories[0].token;
    result.memories[0].salience = 0;
    result.memories[0].confidence = 1;
    ASSERT_TRUE(village.store.Rehearse(Player, 1, 5000, 5000, 0.2));
    auto const before = *village.store.FindReady(Player);
    ASSERT_TRUE(village.coordinator.Submit(result));
    village.coordinator.Update(5000, 5001);
    auto const* after = village.store.FindReady(Player);
    EXPECT_EQ(after->revision, before.revision + 1);
    EXPECT_TRUE(after->perceptions.empty());
    ASSERT_EQ(after->memories.size(), 2u);
    EXPECT_EQ(after->memories[0].id, 1u);
    EXPECT_EQ(after->memories[0].contentRevision, 2u);
    EXPECT_EQ(after->memories[0].claim, "a correction");
    EXPECT_EQ(after->memories[0].source, job->perceptions[0].value.source);
    EXPECT_EQ(after->memories[0].kind, MemoryKind::HeardStatement);
    EXPECT_LE(after->memories[0].confidence, 0.5);
    EXPECT_DOUBLE_EQ(after->memories[0].salience, before.memories[0].salience);
    EXPECT_EQ(after->memories[1].id, 2u);
    EXPECT_EQ(village.coordinator.Stats().fakeSupersessions, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 1u);
}

TEST(AllesPilotTest, DuplicateTargetsRejectCompleteProposalWithoutPartialReplacement)
{
    Village village;
    ASSERT_NE(village.Add(Player, ExistingBelief()), 0u);
    ASSERT_TRUE(village.Hear(Player, "first correction", 0, 0, 99));
    ASSERT_TRUE(village.Hear(Player, "second correction", 0, 0, 100));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Inspect(Player);
    ASSERT_TRUE(job);
    auto result = MakeFakeProposal(*job, {});
    ASSERT_EQ(result.memories.size(), 2u);
    for (auto& proposal : result.memories)
    {
        proposal.operation = ProposalOperation::Supersede;
        proposal.targetMemoryToken = job->memories[0].token;
    }
    auto const before = *village.store.FindReady(Player);
    ASSERT_TRUE(village.coordinator.Submit(result));
    village.coordinator.Update(5000, 5001);
    auto const* after = village.store.FindReady(Player);
    EXPECT_EQ(after->revision, before.revision);
    EXPECT_EQ(after->nextMemoryId, before.nextMemoryId);
    EXPECT_EQ(after->perceptions.size(), 2u);
    ASSERT_EQ(after->memories.size(), 1u);
    EXPECT_EQ(after->memories[0].claim, before.memories[0].claim);
    EXPECT_EQ(village.coordinator.Stats().invalidResults, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeSupersessions, 0u);
    EXPECT_EQ(village.coordinator.Stats().fallbackMemories, 0u);
}

TEST(AllesPilotTest, QueuedSupersessionCannotOverwriteAReplacementAcceptedInTheMeantime)
{
    Village village;
    auto const generation = village.Add(Player, ExistingBelief());
    ASSERT_NE(generation, 0u);
    ASSERT_TRUE(village.Hear(Player, "first correction", 0, 0, 99));
    ASSERT_TRUE(village.Hear(Player, "still pending", 0, 0, 100));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
    village.coordinator.Update(5000, 5000);
    auto job = village.coordinator.Inspect(Player);
    ASSERT_TRUE(job);
    auto result = MakeFakeProposal(*job, {});
    result.memories[0].operation = ProposalOperation::Supersede;
    result.memories[0].targetMemoryToken = job->memories[0].token;
    ASSERT_TRUE(village.coordinator.Submit(result));
    auto replacement = FormFallback(job->perceptions[0].value, {}, 5000);
    replacement.claim = "newly accepted current wording";
    ASSERT_TRUE(village.store.ApplyMutations(Player, generation, {1}, {{1, 1}},
        {{MemoryMutationKind::Supersede, MemoryTarget{Player, {1, 1}}, replacement}}, 5000, 5000));
    auto const before = *village.store.FindReady(Player);
    village.coordinator.Update(5000, 5001);
    auto const* after = village.store.FindReady(Player);
    EXPECT_EQ(after->revision, before.revision);
    ASSERT_EQ(after->memories.size(), 1u);
    EXPECT_EQ(after->memories[0].claim, "newly accepted current wording");
    EXPECT_EQ(after->memories[0].contentRevision, 2u);
    ASSERT_EQ(after->perceptions.size(), 1u);
    EXPECT_EQ(after->perceptions[0].id, 2u);
    EXPECT_EQ(village.coordinator.Stats().invalidatedJobs, 1u);
    EXPECT_EQ(village.coordinator.Stats().staleResults, 1u);
    EXPECT_EQ(village.coordinator.Stats().fakeSupersessions, 0u);
}

TEST(AllesPilotTest, UnknownOperationOrUngroundedKindUsesAuthoritativeFallback)
{
    for (unsigned invalid = 0; invalid < 3; ++invalid)
    {
        Village village;
        ASSERT_NE(village.Add(Player), 0u);
        ASSERT_TRUE(village.Hear(Player, "I heard a report"));
        village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, false});
        village.coordinator.Update(5000, 5000);
        auto result = MakeFakeProposal(*village.coordinator.Inspect(Player), {});
        if (invalid == 0)
            result.memories[0].operation = ProposalOperation(255);
        else
            result.memories[0].kind = invalid == 1 ? MemoryKind::WitnessedDeath : MemoryKind(255);
        ASSERT_TRUE(village.coordinator.Submit(result));
        village.coordinator.Update(5001, 5001);
        auto const* after = village.store.FindReady(Player);
        ASSERT_EQ(after->memories.size(), 1u);
        EXPECT_EQ(after->memories[0].kind, MemoryKind::HeardStatement);
        EXPECT_EQ(after->memories[0].formation, FormationMode::Fallback);
        EXPECT_EQ(village.coordinator.Stats().invalidResults, 1u);
        EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    }
}

TEST(AllesPilotTest, InvalidatedMemoryContextReleasesSlotWithoutResettingOwnerBucket)
{
    MemoryPolicy policy;
    policy.salienceHalfLifeMs = 100;
    ActorStore store({}, policy);
    PilotCoordinator coordinator(store, policy);
    OwnerSnapshot snapshot;
    snapshot.owner = Player;
    snapshot.revision = 1;
    snapshot.nextMemoryId = 2;
    Memory memory;
    memory.id = 1;
    memory.claim = "an older claim";
    memory.confidence = 0.5;
    memory.salience = 0.7;
    snapshot.memories.push_back(memory);
    auto firstGeneration = store.Activate(Player, 1);
    auto secondGeneration = store.Activate(Other, 1);
    ASSERT_TRUE(firstGeneration);
    ASSERT_TRUE(secondGeneration);
    ASSERT_TRUE(store.FinishLoad(Player, *firstGeneration, snapshot, 0, 0));
    snapshot = {};
    snapshot.owner = Other;
    ASSERT_TRUE(store.FinishLoad(Other, *secondGeneration, snapshot, 0, 0));
    ASSERT_TRUE(coordinator.Track(Player));
    ASSERT_TRUE(coordinator.Track(Other));
    Perception input;
    input.text = "first new report";
    ASSERT_TRUE(store.Observe(Player, input, 0));
    input.text = "second new report";
    input.gameTimeMs = 1;
    ASSERT_TRUE(store.Observe(Other, input, 1));
    coordinator.SetFakeBehavior({FakeMode::Withhold, 0, true});
    coordinator.Update(5001, 5001);
    auto original = *coordinator.Inspect(Player);
    ASSERT_FALSE(original.permitId.empty());
    ASSERT_TRUE(coordinator.Inspect(Other)->permitId.empty());
    store.Decay(5002, 5002);
    coordinator.Update(5002, 5002);
    EXPECT_EQ(coordinator.Stats().invalidatedJobs, 1u);
    auto replacement = coordinator.Inspect(Player);
    ASSERT_TRUE(replacement);
    EXPECT_NE(replacement->jobToken, original.jobToken);
    EXPECT_EQ(replacement->admittedRealTimeMs, original.admittedRealTimeMs);
    EXPECT_EQ(replacement->admittedGameTimeMs, original.admittedGameTimeMs);
    EXPECT_TRUE(replacement->permitId.empty());
    EXPECT_FALSE(coordinator.Inspect(Other)->permitId.empty());
    EXPECT_TRUE(store.FindReady(Player)->memories.empty());
    EXPECT_EQ(store.FindReady(Player)->perceptions.size(), 1u);
    ASSERT_TRUE(coordinator.Submit(MakeFakeProposal(*coordinator.Inspect(Other), policy)));
    coordinator.Update(5003, 5003);
    EXPECT_TRUE(coordinator.Inspect(Player)->permitId.empty());
    EXPECT_EQ(coordinator.Stats().dispatched, 2u);
    coordinator.Update(20000, 20000);
    EXPECT_TRUE(coordinator.Inspect(Player)->permitId.empty());
    coordinator.Update(20001, 20001);
    EXPECT_FALSE(coordinator.Inspect(Player)->permitId.empty());
}

TEST(AllesPilotTest, LoadingOwnerCannotBeForgottenWithHiddenBufferedInputs)
{
    ActorStore store;
    PilotCoordinator coordinator(store);
    ASSERT_TRUE(store.Activate(Player, 1));
    ASSERT_TRUE(coordinator.Track(Player));
    Perception input;
    input.text = "heard while loading";
    ASSERT_TRUE(store.Observe(Player, input, 0));
    EXPECT_FALSE(store.FindReady(Player));
    EXPECT_FALSE(coordinator.Forget(Player));
}

TEST(AllesPilotTest, EarlyReceiptDoesNotExtendFakePermitLifetime)
{
    Village village;
    ASSERT_NE(village.Add(Player), 0u);
    ASSERT_TRUE(village.Hear(Player, "waiting for a response"));
    village.coordinator.SetFakeBehavior({FakeMode::Withhold, 0, true});
    village.coordinator.Update(5000, 5000);
    for (uint64_t now = 10000; now <= 25000; now += 5000)
        village.coordinator.Update(now, now);
    auto const job = *village.coordinator.Inspect(Player);
    ASSERT_EQ(job.permitExpiresRealTimeMs, 30000u);
    ASSERT_GT(job.leaseExpiresRealTimeMs, job.permitExpiresRealTimeMs);
    ASSERT_TRUE(village.coordinator.Submit(MakeFakeProposal(job, {})));
    // No update processes this queued receipt until the exact permit-expiry boundary.
    village.coordinator.Update(30000, 30000);
    EXPECT_EQ(village.coordinator.Stats().fakeMemories, 0u);
    EXPECT_EQ(village.coordinator.Stats().fallbackMemories, 1u);
    EXPECT_EQ(village.coordinator.Stats().expiredJobs, 1u);
    EXPECT_EQ(village.coordinator.Stats().staleResults, 1u);
}

}
}
