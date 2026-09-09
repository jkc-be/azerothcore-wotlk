/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Objective.h"
#include "storage/PlanningCodec.h"
#include "gtest/gtest.h"

namespace Alles
{
TEST(AllesActivityObjective, ExhaustedWorkDoesNotSuppressAnotherPurposeAtTheSamePlace)
{
    ObjectiveBook book;
    auto const* work = book.ProposePlace(9, "Find work", "A known place");
    ASSERT_NE(work, nullptr);
    ASSERT_TRUE(book.ActivatePlace(work->id, work->revision, 1000, 1));
    ASSERT_TRUE(book.Block(work->id, Obstruction::Executor, "No work found", 2000));
    ASSERT_TRUE(book.Defer(work->id, 2000));
    auto const* discovery = book.ProposeActivity(9, PlacePurpose::Discovery, "Look around", "An unseen place");
    auto const* rest = book.ProposeActivity(9, PlacePurpose::Rest, "Rest", "I need a break");
    auto const* social = book.ProposeActivity(9, PlacePurpose::Companionship, "Meet a companion", "A known meeting",
        ActorKey{ActorKind::Player, 43});
    ASSERT_NE(discovery, nullptr);
    ASSERT_NE(rest, nullptr);
    ASSERT_NE(social, nullptr);
    EXPECT_NE(discovery->id, work->id);
    EXPECT_NE(rest->id, discovery->id);
    EXPECT_NE(social->id, rest->id);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    EXPECT_EQ(book.ProposeActivity(9, PlacePurpose::Rest, "Rest", "A new reason")->id, rest->id);
    EXPECT_FALSE(book.ProposeActivity(9, PlacePurpose::Companionship, "Meet", "No known person"));
}

TEST(AllesActivityObjective, DiscoveryNeedsANewObservationAndCannotReplayOnArrivalOrReload)
{
    ObjectiveBook book;
    auto const* activity = book.ProposeActivity(9, PlacePurpose::Discovery, "Discover", "My own curiosity");
    ASSERT_NE(activity, nullptr);
    auto const id = activity->id;
    ASSERT_TRUE(book.ActivatePlace(id, activity->revision, 1000, 1));
    ASSERT_TRUE(book.ObserveActivity(id, {9, true}, 2000));
    EXPECT_EQ(activity->state, ObjectiveState::Active);
    EXPECT_FALSE(book.ObservePlace(id, 9, 123, ObjectiveStep::Attempt, 2001));
    ASSERT_TRUE(book.ObserveActivity(id, {9, true, false, true, false}, 3000));
    EXPECT_EQ(activity->state, ObjectiveState::Completed);
    EXPECT_EQ(activity->discoveredQuest, 0u);
    EXPECT_FALSE(book.ReconsiderActivity(id, 3600000));
    PrivateKnowledge knowledge;
    knowledge.Seed(1, false, true);
    PlanningSnapshot snapshot{{ActorKind::Player, 42}, 1, book.Capture(), knowledge.Capture()};
    auto saved = Storage::DecodePlanning(Storage::EncodePlanning(snapshot), snapshot.owner);
    ASSERT_TRUE(saved);
    ObjectiveBook loaded;
    ASSERT_TRUE(loaded.Restore(saved->objectives));
    EXPECT_FALSE(loaded.ReconcilePlace(id, false, 4000));
    EXPECT_EQ(loaded.Find(id)->state, ObjectiveState::Completed);
    EXPECT_FALSE(loaded.ObserveActivity(id, {9, true, false, true, false}, 4000));
    EXPECT_EQ(loaded.ProposeActivity(9, PlacePurpose::Discovery, "Discover", "Another suggestion")->id, id);
}

TEST(AllesActivityObjective, RestCountsOnlyObservedStationaryTimeAndRetainsACooldown)
{
    ObjectiveBook book;
    auto const* rest = book.ProposeActivity(9, PlacePurpose::Rest, "Rest", "Tired");
    ASSERT_NE(rest, nullptr);
    auto const id = rest->id;
    ASSERT_TRUE(book.ActivatePlace(id, rest->revision, 1000, 1));
    for (uint64_t now = 2000; now <= 31000; now += 1000)
        ASSERT_TRUE(book.ObserveActivity(id, {9, true, true}, now));
    EXPECT_EQ(rest->activityMs, 29000u);
    ASSERT_TRUE(book.ObserveActivity(id, {9, false, false}, 32000));
    ASSERT_TRUE(book.ObserveActivity(id, {9, true, true}, 33000));
    EXPECT_EQ(rest->activityMs, 29000u);
    auto const snapshot = book.Capture();
    ASSERT_TRUE(book.Restore(snapshot));
    ASSERT_TRUE(book.ActivatePlace(id, book.Find(id)->revision, 1000000, 1));
    ASSERT_TRUE(book.ObserveActivity(id, {9, true, true}, 1001000));
    EXPECT_EQ(book.Find(id)->activityMs, 29000u);
    for (uint64_t now = 1002000; now <= 1032000; now += 1000)
        ASSERT_TRUE(book.ObserveActivity(id, {9, true, true}, now));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Completed);
    EXPECT_FALSE(book.ReconsiderActivity(id, 1032001));
    ASSERT_TRUE(book.ReconsiderActivity(id, 1632000));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Proposed);
    EXPECT_EQ(book.Find(id)->activityMs, 0u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

TEST(AllesActivityObjective, CompanionshipRequiresInteractionWithTheIntendedPerson)
{
    ObjectiveBook book;
    auto const* visit = book.ProposeActivity(9, PlacePurpose::Companionship, "Visit", "Known companion",
        ActorKey{ActorKind::Player, 43});
    ASSERT_NE(visit, nullptr);
    auto const id = visit->id;
    ASSERT_TRUE(book.ActivatePlace(id, visit->revision, 1000, 1));
    ASSERT_TRUE(book.ObserveActivity(id, {9, true}, 2000));
    EXPECT_EQ(visit->state, ObjectiveState::Active);
    ASSERT_TRUE(book.ObserveActivity(id, {9, true, false, false, true, ActorKey{ActorKind::Player, 44}}, 3000));
    EXPECT_EQ(visit->state, ObjectiveState::Active);
    ASSERT_TRUE(book.ObserveActivity(id, {9, true, false, false, true, ActorKey{ActorKind::Player, 43}}, 4000));
    EXPECT_EQ(visit->state, ObjectiveState::Completed);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

TEST(AllesActivityObjective, ReplanningRetainsIntentWithoutClaimingFailureOrCompletion)
{
    ObjectiveBook book;
    auto const* activity = book.ProposeActivity(9, PlacePurpose::Discovery, "Explore", "Curious");
    ASSERT_NE(activity, nullptr);
    auto const id = activity->id;
    ASSERT_TRUE(book.ActivatePlace(id, activity->revision, 1000, 1));
    ASSERT_TRUE(book.Replan(id, "Observed danger makes another activity preferable", 2000));
    EXPECT_EQ(activity->state, ObjectiveState::Deferred);
    EXPECT_EQ(activity->obstruction, Obstruction::None);
    EXPECT_FALSE(book.Retryable(*activity, 3000, 1));
    EXPECT_TRUE(book.Retryable(*activity, 32000, 1));
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

TEST(AllesActivityObjective, QuestCounterRollbackAndReloadCannotAwardTheSameCreditAgain)
{
    ObjectiveBook book;
    auto const* objective = book.ProposeQuest(123, "Quest", "Accepted work");
    ASSERT_NE(objective, nullptr);
    auto const id = objective->id;
    QuestProgress progress;
    progress.inLog = true;
    progress.counters[0] = 2;
    EXPECT_EQ(book.AccountQuestProgress(id, progress), (std::pair<uint32_t, bool>{0, false}));
    progress.counters[0] = 4;
    EXPECT_EQ(book.AccountQuestProgress(id, progress), (std::pair<uint32_t, bool>{2, false}));
    ASSERT_TRUE(book.Restore(book.Capture()));
    progress.counters[0] = 2;
    EXPECT_EQ(book.AccountQuestProgress(id, progress), (std::pair<uint32_t, bool>{0, false}));
    progress.counters[0] = 4;
    EXPECT_EQ(book.AccountQuestProgress(id, progress), (std::pair<uint32_t, bool>{0, false}));
    progress.rewarded = true;
    EXPECT_EQ(book.AccountQuestProgress(id, progress), (std::pair<uint32_t, bool>{0, true}));
    ASSERT_TRUE(book.Restore(book.Capture()));
    EXPECT_EQ(book.AccountQuestProgress(id, progress), (std::pair<uint32_t, bool>{0, false}));
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}
TEST(AllesActivityObjective, WorkSuggestionsCannotExcludeEveryNonWorkPurpose)
{
    ObjectiveBook book;
    for (uint32_t area = 1; area <= 32; ++area)
        ASSERT_NE(book.ProposePlace(area, "Look for work", "Known area"), nullptr);
    auto const* rest = book.ProposeActivity(1, PlacePurpose::Rest, "Rest", "Tired");
    ASSERT_NE(rest, nullptr);
    EXPECT_EQ(book.All().size(), 32u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

}
