/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "ActorStore.h"
#include "gtest/gtest.h"

namespace Alles
{
namespace
{
constexpr ActorKey Owner{ActorKind::Player, 42};

uint64_t Load(ActorStore& store, ActorKey owner = Owner)
{
    auto const generation = store.Activate(owner, 1);
    EXPECT_TRUE(generation);
    OwnerSnapshot snapshot;
    snapshot.owner = owner;
    EXPECT_TRUE(store.FinishLoad(owner, generation.value_or(0), snapshot, 0, 0));
    return generation.value_or(0);
}
}

TEST(AllesPlanningStore, IndependentPlanningRevisionPreservesConcurrentMemoryIngress)
{
    ActorStore store;
    auto const generation = Load(store);
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Seed(1, false, true));
    auto revision = store.UpdatePlanning(Owner, generation, 0, {}, knowledge.Capture(), 1000);
    ASSERT_TRUE(revision);
    EXPECT_EQ(*revision, 1u);
    Perception speech;
    speech.text = "A remembered conversation";
    ASSERT_TRUE(store.Observe(Owner, speech, 2000));
    ASSERT_TRUE(knowledge.Visit(9, "Northshire Valley", 2000, true));
    revision = store.UpdatePlanning(Owner, generation, 1, {}, knowledge.Capture(), 2000);
    ASSERT_TRUE(revision);
    EXPECT_EQ(*revision, 2u);
    auto const* snapshot = store.FindReady(Owner);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->perceptions.size(), 1u);
    EXPECT_EQ(snapshot->perceptions[0].text, speech.text);
    EXPECT_EQ(snapshot->planning->knowledge, knowledge.Capture());
    auto const outerRevision = snapshot->revision;
    EXPECT_EQ(store.UpdatePlanning(Owner, generation, 2, {}, knowledge.Capture(), 3000), 2u);
    EXPECT_EQ(snapshot->revision, outerRevision);
}

TEST(AllesPlanningStore, SatisfactionSharesAtomicRevisionAndCannotBeLostByAnUnrelatedUpdate)
{
    ActorStore store;
    auto const generation = Load(store);
    SatisfactionModel model;
    ASSERT_TRUE(model.Observe(1000, 0, {{"discovery", 0.3}}));
    ASSERT_EQ(store.UpdatePlanning(Owner, generation, 0, {}, {}, 1000, model.Capture()), 1u);
    EXPECT_EQ(store.FindReady(Owner)->planning->satisfaction, model.Capture());
    auto const first = model.Capture();
    ASSERT_TRUE(model.Observe(2000, 1000, {{"companionship", 0.1}}));
    EXPECT_FALSE(store.UpdatePlanning(Owner, generation, 0, {}, {}, 2000, model.Capture()));
    EXPECT_EQ(store.FindReady(Owner)->planning->satisfaction, first);
    ASSERT_EQ(store.UpdatePlanning(Owner, generation, 1, {}, {}, 2000, model.Capture()), 2u);
    EXPECT_EQ(store.UpdatePlanning(Owner, generation, 2, {}, {}, 3000), 2u);
    EXPECT_EQ(store.FindReady(Owner)->planning->satisfaction, model.Capture());
    auto corrupt = model.Capture();
    corrupt.dimensions.begin()->second.fulfillment = -1;
    EXPECT_FALSE(store.UpdatePlanning(Owner, generation, 2, {}, {}, 3000, corrupt));
    EXPECT_EQ(store.FindReady(Owner)->planning->satisfaction, model.Capture());
}

TEST(AllesPlanningStore, StaleGenerationRevisionAndMalformedStateAreAtomicRejections)
{
    ActorStore store;
    auto const generation = Load(store);
    auto const revision = store.UpdatePlanning(Owner, generation, 0, {}, {}, 1000);
    ASSERT_TRUE(revision);
    auto const previous = store.FindReady(Owner)->planning;
    auto const outerRevision = store.FindReady(Owner)->revision;
    EXPECT_FALSE(store.UpdatePlanning(Owner, generation + 1, 1, {}, {}, 2000));
    EXPECT_FALSE(store.UpdatePlanning(Owner, generation, 0, {}, {}, 2000));
    KnowledgeSnapshot corrupt;
    corrupt.seedVersion = 2;
    EXPECT_FALSE(store.UpdatePlanning(Owner, generation, 1, {}, corrupt, 2000));
    EXPECT_EQ(store.FindReady(Owner)->planning, previous);
    EXPECT_EQ(store.FindReady(Owner)->revision, outerRevision);
}

TEST(AllesPlanningStore, InFlightSaveCannotOverwriteNewerIntentAndReloadKeepsPrivateKnowledge)
{
    ActorStore store;
    auto const generation = Load(store);
    PrivateKnowledge knowledge;
    knowledge.Seed(1, false, true);
    ASSERT_TRUE(store.UpdatePlanning(Owner, generation, 0, {}, knowledge.Capture(), 1000));
    store.RequestFlush(Owner);
    auto first = store.CaptureSave(Owner, 10000);
    ASSERT_TRUE(first);
    knowledge.Hear({ActorKey{ActorKind::Player, 43}, "Speaker"},
        {Activity::Hunt, 0, 87, {}}, "There might be useful prey", 11000, 0.4);
    ASSERT_TRUE(store.UpdatePlanning(Owner, generation, 1, {}, knowledge.Capture(), 11000));
    EXPECT_TRUE(first->snapshot.planning->knowledge.reports.empty());
    EXPECT_TRUE(store.CompleteSave(Owner, generation, first->snapshot.revision, true, 12000));
    EXPECT_EQ(store.FindReady(Owner)->planning->revision, 2u);
    store.RequestFlush(Owner);
    auto second = store.CaptureSave(Owner, 20000);
    ASSERT_TRUE(second);
    ActorStore reloaded;
    auto loadedGeneration = reloaded.Activate(Owner, 2);
    ASSERT_TRUE(loadedGeneration);
    ASSERT_TRUE(reloaded.FinishLoad(Owner, *loadedGeneration, second->snapshot, 21000, 0));
    EXPECT_EQ(reloaded.FindReady(Owner)->planning->knowledge, knowledge.Capture());
    ActorKey stranger{ActorKind::CreatureSpawn, Owner.id};
    Load(reloaded, stranger);
    EXPECT_FALSE(reloaded.FindReady(stranger)->planning);
}

TEST(AllesPlanningStore, ForeignOwnerAndNewerPlanningWatermarkRejectTheEntireLoad)
{
    ActorStore store;
    auto generation = store.Activate(Owner, 1);
    ASSERT_TRUE(generation);
    OwnerSnapshot snapshot;
    snapshot.owner = Owner;
    snapshot.revision = 1;
    snapshot.planning = PlanningSnapshot{{ActorKind::Player, 43}, 1, {}, {}};
    EXPECT_FALSE(store.FinishLoad(Owner, *generation, snapshot, 1000, 0));
    snapshot.planning->owner = Owner;
    snapshot.planning->revision = 2;
    EXPECT_FALSE(store.FinishLoad(Owner, *generation, snapshot, 1000, 0));
    EXPECT_EQ(store.Status(Owner)->state, ActorState::Loading);
}
}
