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
constexpr ActorKey Owner{ActorKind::Player, 42};
constexpr ActorKey OtherOwner{ActorKind::CreatureSpawn, 42};
constexpr ActorKey Source{ActorKind::Player, 43};

Memory Belief(uint64_t id, std::string claim, double salience = 0.4)
{
    Memory memory;
    memory.id = id;
    memory.claim = std::move(claim);
    memory.source = {Source, "Humanc"};
    memory.confidence = 0.3;
    memory.salience = salience;
    return memory;
}

struct Fixture
{
    ActorStore store;
    uint64_t generation = 0;

    bool Load(OwnerSnapshot snapshot = {})
    {
        snapshot.owner = Owner;
        if (snapshot.memories.empty())
        {
            snapshot.memories = {Belief(1, "first belief"), Belief(2, "second belief", 0.6)};
            snapshot.nextMemoryId = 3;
        }
        auto value = store.Activate(Owner, 1);
        if (!value)
            return false;
        generation = *value;
        return store.FinishLoad(Owner, generation, std::move(snapshot), 0, 0);
    }

    bool Observe(std::string text)
    {
        Perception input;
        input.text = std::move(text);
        input.source = {Source, "Humanc"};
        return store.Observe(Owner, std::move(input), 0);
    }

    OwnerSnapshot Snapshot() const { return *store.FindReady(Owner); }
};

MemoryMutation Target(MemoryMutationKind kind, uint64_t id, Memory memory,
    ActorKey owner = Owner, uint64_t revision = 1)
{
    return {kind, MemoryTarget{owner, {id, revision}}, std::move(memory)};
}

void ExpectUnchanged(OwnerSnapshot const& before, OwnerSnapshot const& after)
{
    EXPECT_EQ(after.owner, before.owner);
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_EQ(after.nextMemoryId, before.nextMemoryId);
    EXPECT_EQ(after.nextPerceptionId, before.nextPerceptionId);
    EXPECT_EQ(after.decayGameTimeMs, before.decayGameTimeMs);
    EXPECT_EQ(after.nextConsolidationGameTimeMs, before.nextConsolidationGameTimeMs);
    ASSERT_EQ(after.perceptions.size(), before.perceptions.size());
    for (std::size_t index = 0; index < before.perceptions.size(); ++index)
    {
        EXPECT_EQ(after.perceptions[index].id, before.perceptions[index].id);
        EXPECT_EQ(after.perceptions[index].text, before.perceptions[index].text);
    }
    ASSERT_EQ(after.memories.size(), before.memories.size());
    for (std::size_t index = 0; index < before.memories.size(); ++index)
    {
        auto const& left = before.memories[index];
        auto const& right = after.memories[index];
        EXPECT_EQ(right.id, left.id);
        EXPECT_EQ(right.contentRevision, left.contentRevision);
        EXPECT_EQ(right.kind, left.kind);
        EXPECT_EQ(right.claim, left.claim);
        EXPECT_EQ(right.subject, left.subject);
        EXPECT_EQ(right.source, left.source);
        EXPECT_EQ(right.attribution, left.attribution);
        EXPECT_EQ(right.reportedDepth, left.reportedDepth);
        EXPECT_DOUBLE_EQ(right.salience, left.salience);
        EXPECT_DOUBLE_EQ(right.confidence, left.confidence);
        EXPECT_EQ(right.formedGameTimeMs, left.formedGameTimeMs);
        EXPECT_EQ(right.recalledGameTimeMs, left.recalledGameTimeMs);
        EXPECT_EQ(right.decayGameTimeMs, left.decayGameTimeMs);
        EXPECT_EQ(right.formation, left.formation);
    }
}

TEST(AllesMemoryMutationTest, MixedCreateReinforceSupersedePublishesOneRevision)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("first belief"));
    ASSERT_TRUE(fixture.Observe("a correction"));
    ASSERT_TRUE(fixture.Observe("new belief"));
    auto const before = fixture.Snapshot();
    auto reinforced = before.memories[0];
    reinforced.salience = 0;
    reinforced.confidence = 0.6;
    reinforced.source = {ActorKey{ActorKind::Player, 99}, "A new speaker"};
    auto replacement = Belief(0, "corrected second belief", 1);
    replacement.confidence = 0.5;
    replacement.formation = FormationMode::InProcessFake;
    std::vector<MemoryMutation> mutations =
    {
        {MemoryMutationKind::Create, std::nullopt, Belief(0, "new belief")},
        Target(MemoryMutationKind::Reinforce, 1, reinforced),
        Target(MemoryMutationKind::Supersede, 2, replacement)
    };
    ASSERT_TRUE(fixture.store.ApplyMutations(Owner, fixture.generation, {1, 2, 3}, {{1, 1}, {2, 1}}, mutations, 0, 1));
    auto const after = fixture.Snapshot();
    EXPECT_EQ(after.revision, before.revision + 1);
    EXPECT_TRUE(after.perceptions.empty());
    ASSERT_EQ(after.memories.size(), 3u);
    EXPECT_EQ(after.nextMemoryId, 4u);
    EXPECT_EQ(after.memories[0].claim, before.memories[0].claim);
    EXPECT_EQ(after.memories[0].source, before.memories[0].source);
    EXPECT_DOUBLE_EQ(after.memories[0].confidence, before.memories[0].confidence);
    EXPECT_DOUBLE_EQ(after.memories[0].salience, 0.5);
    EXPECT_EQ(after.memories[0].contentRevision, 1u);
    EXPECT_EQ(after.memories[1].id, 2u);
    EXPECT_EQ(after.memories[1].claim, "corrected second belief");
    EXPECT_EQ(after.memories[1].contentRevision, 2u);
    EXPECT_DOUBLE_EQ(after.memories[1].salience, 0.6);
    EXPECT_DOUBLE_EQ(after.memories[1].confidence, 0.5);
    EXPECT_EQ(after.memories[2].id, 3u);
}

TEST(AllesMemoryMutationTest, BadTargetRejectsEarlierValidCreateWithoutAnyChanges)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("an input"));
    auto const before = fixture.Snapshot();
    std::vector<MemoryMutation> mutations =
    {
        {MemoryMutationKind::Create, std::nullopt, Belief(0, "must not appear")},
        Target(MemoryMutationKind::Supersede, 2, Belief(0, "bad revision"), Owner, 99)
    };
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}, {2, 1}}, mutations, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());
    mutations[1] = Target(MemoryMutationKind::Supersede, 1, Belief(0, "foreign"), OtherOwner);
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}}, mutations, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());
    mutations[1] = Target(MemoryMutationKind::Supersede, 999, Belief(0, "missing"));
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{999, 1}}, mutations, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());
}

TEST(AllesMemoryMutationTest, DuplicateTargetsAndDuplicateSuppliedVersionsRejectWholeBatch)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("pending"));
    auto const before = fixture.Snapshot();
    std::vector<MemoryMutation> mutations =
    {
        Target(MemoryMutationKind::Reinforce, 1, before.memories[0]),
        Target(MemoryMutationKind::Supersede, 1, Belief(0, "replacement"))
    };
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}}, mutations, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());
    mutations.pop_back();
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}, {1, 1}}, mutations, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());
    mutations.front().kind = MemoryMutationKind(255);
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}}, mutations, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());
}

TEST(AllesMemoryMutationTest, ReinforcementPreservesForgottenProvenanceAndCurrentSalience)
{
    Fixture fixture;
    OwnerSnapshot snapshot;
    auto eroded = Belief(1, "something, but I no longer remember what or from whom");
    eroded.source = {};
    eroded.subject = {};
    eroded.confidence = 0.2;
    snapshot.memories = {eroded};
    snapshot.nextMemoryId = 2;
    ASSERT_TRUE(fixture.Load(snapshot));
    ASSERT_TRUE(fixture.Observe("reminder"));
    ASSERT_TRUE(fixture.store.Rehearse(Owner, 1, 0, 1, 0.3));
    auto const before = fixture.Snapshot();
    auto suggested = eroded;
    suggested.source = {Source, "Humanc"};
    suggested.attribution = "an invented origin";
    suggested.reportedDepth = 1;
    suggested.confidence = 0.6;
    suggested.salience = 0;
    ASSERT_TRUE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}},
        {Target(MemoryMutationKind::Reinforce, 1, suggested)}, 0, 2));
    auto const& after = fixture.store.FindReady(Owner)->memories[0];
    EXPECT_EQ(after.source, Reference{});
    EXPECT_TRUE(after.attribution.empty());
    EXPECT_FALSE(after.reportedDepth);
    EXPECT_EQ(after.claim, eroded.claim);
    EXPECT_DOUBLE_EQ(after.confidence, 0.2);
    EXPECT_DOUBLE_EQ(after.salience, before.memories[0].salience + 0.1);
}

TEST(AllesMemoryMutationTest, SupersedeUsesCurrentSalienceAndOlderSaveCannotRestoreOldContent)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("correction"));
    fixture.store.RequestFlush(Owner);
    auto older = fixture.store.CaptureSave(Owner, 0);
    ASSERT_TRUE(older);
    ASSERT_TRUE(fixture.store.Rehearse(Owner, 1, 0, 1, 0.4));
    auto const salience = fixture.Snapshot().memories[0].salience;
    auto replacement = Belief(0, "a corrected belief", 0.1);
    ASSERT_TRUE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}},
        {Target(MemoryMutationKind::Supersede, 1, replacement)}, 0, 2));
    ASSERT_TRUE(fixture.store.CompleteSave(Owner, fixture.generation, older->snapshot.revision, true, 3));
    auto const after = fixture.Snapshot();
    EXPECT_EQ(after.memories[0].claim, "a corrected belief");
    EXPECT_EQ(after.memories[0].contentRevision, 2u);
    EXPECT_DOUBLE_EQ(after.memories[0].salience, salience);
    EXPECT_LT(fixture.store.Status(Owner)->committedRevision, after.revision);
    EXPECT_EQ(older->snapshot.memories[0].claim, "first belief");
}

TEST(AllesMemoryMutationTest, StagedDecayCannotResurrectOrPartiallyApply)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("pending"));
    auto const before = fixture.Snapshot();
    auto const longAfter = uint64_t(100) * 24 * 60 * 60 * 1000;
    std::vector<MemoryMutation> mutations =
    {
        {MemoryMutationKind::Create, std::nullopt, Belief(0, "must remain absent")},
        Target(MemoryMutationKind::Reinforce, 1, before.memories[0])
    };
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}}, mutations, longAfter, 1));
    ExpectUnchanged(before, fixture.Snapshot());
}

TEST(AllesMemoryMutationTest, ExhaustedIdsAndContentRevisionsRejectBeforeCommit)
{
    Fixture ids;
    OwnerSnapshot snapshot;
    snapshot.memories = {Belief(1, "first belief")};
    snapshot.nextMemoryId = std::numeric_limits<uint64_t>::max() - 1;
    ASSERT_TRUE(ids.Load(snapshot));
    ASSERT_TRUE(ids.Observe("pending"));
    auto before = ids.Snapshot();
    EXPECT_FALSE(ids.store.ApplyMutations(Owner, ids.generation, {1}, {},
        {{MemoryMutationKind::Create, std::nullopt, Belief(0, "first")},
            {MemoryMutationKind::Create, std::nullopt, Belief(0, "overflow")}}, 0, 1));
    ExpectUnchanged(before, ids.Snapshot());
    ASSERT_TRUE(ids.store.ApplyMutations(Owner, ids.generation, {1}, {},
        {{MemoryMutationKind::Create, std::nullopt, Belief(0, "final usable ID")}}, 0, 1));
    EXPECT_EQ(ids.Snapshot().memories.back().id, std::numeric_limits<uint64_t>::max() - 1);
    EXPECT_EQ(ids.Snapshot().nextMemoryId, std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(ids.Observe("another input"));
    before = ids.Snapshot();
    EXPECT_FALSE(ids.store.ApplyMutations(Owner, ids.generation, {2}, {},
        {{MemoryMutationKind::Create, std::nullopt, Belief(0, "exhausted")}}, 0, 2));
    ExpectUnchanged(before, ids.Snapshot());

    Fixture revisions;
    snapshot.nextMemoryId = 2;
    snapshot.memories[0].contentRevision = std::numeric_limits<uint64_t>::max() - 1;
    ASSERT_TRUE(revisions.Load(snapshot));
    ASSERT_TRUE(revisions.Observe("pending"));
    before = revisions.Snapshot();
    auto const revision = snapshot.memories[0].contentRevision;
    EXPECT_FALSE(revisions.store.ApplyMutations(Owner, revisions.generation, {1}, {{1, revision}},
        {Target(MemoryMutationKind::Supersede, 1, Belief(0, "overflow"), Owner, revision)}, 0, 1));
    ExpectUnchanged(before, revisions.Snapshot());
}

TEST(AllesMemoryMutationTest, ForgottenAndSupersededTargetsCannotBeResurrected)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("first correction"));
    ASSERT_TRUE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}},
        {Target(MemoryMutationKind::Supersede, 1, Belief(0, "new current belief"))}, 0, 1));
    ASSERT_TRUE(fixture.Observe("later correction"));
    auto before = fixture.Snapshot();
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {2}, {{1, 1}},
        {Target(MemoryMutationKind::Supersede, 1, Belief(0, "old delayed belief"))}, 0, 2));
    ExpectUnchanged(before, fixture.Snapshot());
    fixture.store.Decay(uint64_t(100) * 24 * 60 * 60 * 1000, 3);
    before = fixture.Snapshot();
    ASSERT_TRUE(before.memories.empty());
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {2}, {{1, 2}},
        {Target(MemoryMutationKind::Supersede, 1, Belief(0, "resurrected"), Owner, 2)}, 0, 4));
    ExpectUnchanged(before, fixture.Snapshot());
}

TEST(AllesMemoryMutationTest, RewritingReinforcementAndSnapshotRevisionExhaustionAreAtomic)
{
    Fixture fixture;
    ASSERT_TRUE(fixture.Load());
    ASSERT_TRUE(fixture.Observe("pending"));
    auto before = fixture.Snapshot();
    EXPECT_FALSE(fixture.store.ApplyMutations(Owner, fixture.generation, {1}, {{1, 1}},
        {Target(MemoryMutationKind::Reinforce, 1, Belief(0, "rewritten claim"))}, 0, 1));
    ExpectUnchanged(before, fixture.Snapshot());

    Fixture exhausted;
    before.revision = std::numeric_limits<uint64_t>::max() - 1;
    ASSERT_TRUE(exhausted.Load(before));
    auto const full = exhausted.Snapshot();
    EXPECT_FALSE(exhausted.store.ApplyMutations(Owner, exhausted.generation, {1}, {{1, 1}},
        {Target(MemoryMutationKind::Reinforce, 1, full.memories[0])}, 0, 1));
    ExpectUnchanged(full, exhausted.Snapshot());
}
}
}
