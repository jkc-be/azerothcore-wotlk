/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Exploration.h"
#include "Objective.h"
#include "gtest/gtest.h"

namespace Alles
{
TEST(AllesExploration, ExhaustionRequiresActualRepeatedSearchAndSpatialInvestigation)
{
    LocalSurvey stationary;
    LocalSurvey travelling;
    LocalSurvey searching;
    for (uint64_t now = 1000; now <= 125000; now += 1000)
    {
        auto const scan = now / 5000 + 1;
        float const x = float(now / 1000);
        stationary.Observe(now, scan, true, true, 0, 0);
        travelling.Observe(now, scan, true, false, x, 0);
        searching.Observe(now, scan, true, true, x, 0);
    }
    EXPECT_FALSE(stationary.Exhausted());
    EXPECT_FALSE(travelling.Exhausted());
    EXPECT_TRUE(searching.Exhausted());
}

TEST(AllesExploration, AStaleScanOfflineGapOrAvailableWorkCannotEstablishExhaustion)
{
    LocalSurvey survey;
    for (uint64_t now = 1000; now <= 125000; now += 1000)
        survey.Observe(now, 1, true, true, float(now), 0);
    EXPECT_FALSE(survey.Exhausted());
    EXPECT_LE(survey.ActiveMs(), 10000u);
    auto const before = survey.ActiveMs();
    survey.Observe(1000000, 2, true, true, 100, 0);
    EXPECT_EQ(survey.ActiveMs(), before);
    survey.Observe(1001000, 3, false, true, 100, 0);
    EXPECT_EQ(survey.ActiveMs(), 0u);
    EXPECT_EQ(survey.EmptyScans(), 0u);
    EXPECT_EQ(survey.Positions(), 0u);
}

TEST(AllesExploration, ArrivalIsNotCompletionAndWorkMustBeObservedInTheIntendedArea)
{
    ObjectiveBook book;
    auto const* place = book.ProposePlace(87, "Find work in Goldshire", "Explore known surroundings");
    ASSERT_NE(place, nullptr);
    ASSERT_TRUE(book.ActivatePlace(place->id, place->revision, 1000, 5));
    EXPECT_FALSE(book.Activate(place->id, place->revision, {}, 1000, 5));
    ASSERT_TRUE(book.ObservePlace(place->id, 9, 42, ObjectiveStep::Travel, 2000));
    EXPECT_EQ(place->arrivedMs, 0u);
    EXPECT_EQ(place->state, ObjectiveState::Active);
    ASSERT_TRUE(book.ObservePlace(place->id, 87, 0, ObjectiveStep::Attempt, 3000));
    EXPECT_EQ(place->arrivedMs, 3000u);
    EXPECT_EQ(place->state, ObjectiveState::Active);
    ASSERT_TRUE(book.ObservePlace(place->id, 87, 42, ObjectiveStep::Attempt, 4000));
    EXPECT_EQ(place->state, ObjectiveState::Completed);
    EXPECT_EQ(place->discoveredQuest, 42u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

TEST(AllesExploration, ReloadAndDeferralRetainPlaceIntentWithoutResumingOldMovement)
{
    ObjectiveBook book;
    auto const* place = book.ProposePlace(87, "Find work", "Known nearby settlement");
    ASSERT_NE(place, nullptr);
    auto const id = place->id;
    book.ActivatePlace(id, place->revision, 1000, 5);
    book.ObservePlace(id, 87, 0, ObjectiveStep::Attempt, 2000);
    ObjectiveBook reloaded;
    ASSERT_TRUE(reloaded.Restore(book.Capture()));
    EXPECT_EQ(reloaded.Current()->state, ObjectiveState::Waiting);
    EXPECT_FALSE(reloaded.Reconcile(id, {}, 3000));
    reloaded.ObservePlace(id, 87, 42, ObjectiveStep::Wait, 3000);
    EXPECT_EQ(reloaded.Current()->state, ObjectiveState::Waiting);
    reloaded.ActivatePlace(id, reloaded.Current()->revision, 4000, 5);
    reloaded.ObservePlace(id, 87, 42, ObjectiveStep::Attempt, 5000);
    ASSERT_EQ(reloaded.Find(id)->state, ObjectiveState::Completed);
    reloaded.ReconcilePlace(id, false, 6000);
    EXPECT_EQ(reloaded.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_EQ(reloaded.Find(id)->discoveredQuest, 0u);
    EXPECT_TRUE(reloaded.ActivatePlace(id, reloaded.Find(id)->revision, 6000, 5));
}

TEST(AllesExploration, AnUnusableNearbyOfferEventuallyDefersInsteadOfKeepingTheBotForever)
{
    ObjectiveBook book({3000, 6000, 3, 32});
    auto const* place = book.ProposePlace(87, "Find work", "Known settlement");
    ASSERT_NE(place, nullptr);
    book.ActivatePlace(place->id, place->revision, 1000, 5);
    for (uint64_t now = 2000; now <= 5000; now += 1000)
        ASSERT_TRUE(book.ObservePlace(place->id, 87, 0, ObjectiveStep::Attempt, now));
    EXPECT_EQ(place->state, ObjectiveState::Blocked);
    EXPECT_EQ(place->obstruction, Obstruction::Executor);
    EXPECT_EQ(place->discoveredQuest, 0u);
    EXPECT_TRUE(book.Defer(place->id, 5000));
    EXPECT_FALSE(book.ActivatePlace(place->id, place->revision, 6000, 5));
}
}
