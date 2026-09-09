/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "BodyControl.h"
#include "BodyTravel.h"
#include "gtest/gtest.h"

TEST(AllesBody, OldIncarnationsCannotAcquireIssueOrReleaseTheBody)
{
    BodyControl body;
    EXPECT_FALSE(body.Attach(0, 7, 100));
    ASSERT_TRUE(body.Attach(2, 7, 100));
    ASSERT_TRUE(body.Issue(2, 7, 31, BodyControl::Skill::Travel, 200));
    EXPECT_FALSE(body.Attach(2, 8, 300));
    EXPECT_FALSE(body.Issue(1, 7, 32, BodyControl::Skill::Quest, 300));
    EXPECT_FALSE(body.Issue(2, 6, 32, BodyControl::Skill::Quest, 300));
    EXPECT_FALSE(body.Detach(2, 6));
    EXPECT_EQ(body.objective, 31u);
    ASSERT_TRUE(body.Detach(2, 7));
    EXPECT_FALSE(body.Attached());
    ASSERT_TRUE(body.Attach(3, 8, 400));
    EXPECT_EQ(body.skill, BodyControl::Skill::Idle);
    EXPECT_EQ(body.objective, 0u);
}

TEST(AllesBody, PurposeSpecificActivitiesRemainOwnedAndDoNotAuthorizeQuestCombat)
{
    BodyControl body;
    ASSERT_TRUE(body.Attach(1, 2, 1000));
    EXPECT_FALSE(body.Issue(1, 2, 0, BodyControl::Skill::Activity, 1000));
    ASSERT_TRUE(body.Issue(1, 2, 10, BodyControl::Skill::Activity, 1000));
    EXPECT_TRUE(body.Directed());
    EXPECT_EQ(BodyControl::Name(body.skill), "activity");
    EXPECT_FALSE(body.MayStartQuestCombat(10, true, 1001));
    EXPECT_FALSE(body.Issue(2, 2, 11, BodyControl::Skill::Activity, 1001));
    body.Pause(BodyControl::Interrupt::Combat);
    EXPECT_EQ(body.objective, 10u);
    EXPECT_FALSE(body.Fresh(6001));
}

TEST(AllesBody, CombatAndRecoveryRetainIntentionButDoNotResurrectBlockedWork)
{
    BodyControl body;
    body.Attach(1, 1, 0);
    body.Issue(1, 1, 31, BodyControl::Skill::Travel, 0);
    for (auto reason : {BodyControl::Interrupt::Combat, BodyControl::Interrupt::Recovery})
    {
        body.Pause(reason);
        EXPECT_EQ(body.state, BodyControl::State::Interrupted);
        body.Issue(1, 1, 31, BodyControl::Skill::Travel, 1000);
        EXPECT_EQ(body.interruption, reason);
        EXPECT_EQ(body.objective, 31u);
        body.Resume();
        EXPECT_EQ(body.state, BodyControl::State::Running);
    }
    body.state = BodyControl::State::Blocked;
    body.Pause(BodyControl::Interrupt::Combat);
    body.Resume();
    body.Issue(1, 1, 31, BodyControl::Skill::Travel, 2000);
    EXPECT_EQ(body.state, BodyControl::State::Blocked);
    body.Issue(1, 1, 32, BodyControl::Skill::Investigate, 3000);
    EXPECT_EQ(body.state, BodyControl::State::Running);
}

TEST(AllesBody, MissingBrainDoesNotGrantLegacyAutonomyAndHeartbeatHandlesClockWrap)
{
    BodyControl body;
    body.Attach(1, 1, UINT32_MAX - 1000);
    EXPECT_TRUE(body.Fresh(1000));
    EXPECT_FALSE(body.Fresh(6000));
    EXPECT_TRUE(body.Attached());
    EXPECT_FALSE(body.Issue(1, 1, 0, BodyControl::Skill::Travel, 7000));
    EXPECT_FALSE(body.Issue(1, 1, 1, static_cast<BodyControl::Skill>(255), 7000));
}

TEST(AllesBody, OnlyCurrentAttemptingQuestWorkMayStartANewFight)
{
    BodyControl body;
    EXPECT_TRUE(body.MayStartQuestCombat(0, false, 0)); // Ordinary Playerbots retain their existing policy.
    body.Attach(1, 1, 100);
    body.Issue(1, 1, 31, BodyControl::Skill::Travel, 100);
    EXPECT_FALSE(body.MayStartQuestCombat(31, true, 200));
    body.Issue(1, 1, 31, BodyControl::Skill::Quest, 200);
    EXPECT_FALSE(body.MayStartQuestCombat(30, true, 300));
    EXPECT_FALSE(body.MayStartQuestCombat(31, false, 300));
    EXPECT_TRUE(body.MayStartQuestCombat(31, true, 300));
    EXPECT_FALSE(body.MayStartQuestCombat(31, true, 6000));
    body.state = BodyControl::State::Blocked;
    EXPECT_FALSE(body.MayStartQuestCombat(31, true, 400));
}

TEST(AllesBodyTravel, ADetourAwayFromTheDestinationAdvancesAlongTheCommittedRoute)
{
    BodyTravel travel;
    ASSERT_TRUE(travel.Commit({{0, 0, 0}, {-40, 0, 0}, {-40, 40, 0}, {40, 40, 0}, {40, 0, 0}}, 0));
    for (uint32_t step = 1; step <= 40; ++step)
        travel.Observe({-float(step), 0, 0}, step * 1000);
    EXPECT_FALSE(travel.Stalled());
    EXPECT_GT(travel.Advances(), 5u);
    EXPECT_GT(travel.Waypoint(), 1u);
}

TEST(AllesBodyTravel, FastMovementCanPassShortWaypointsBetweenSamples)
{
    BodyTravel travel;
    std::vector<BodyTravel::Point> points;
    for (unsigned i = 0; i <= 50; ++i)
        points.push_back({float(i * 2), 0, 0});
    ASSERT_TRUE(travel.Commit(points, 0));
    for (uint32_t now = 1000; now <= 6000; now += 1000)
        travel.Observe({float(now / 1000 * 14), 0, 0}, now);
    EXPECT_GT(travel.Waypoint(), 35u);
    EXPECT_FALSE(travel.Stalled());
}

TEST(AllesBodyTravel, OscillationAtAnObstacleEventuallyFailsDespiteContinuedMovement)
{
    BodyTravel travel;
    ASSERT_TRUE(travel.Commit({{0, 0, 0}, {100, 0, 0}}, 0));
    for (uint32_t now = 1000; now <= 40000; now += 1000)
        travel.Observe({now % 2000 ? 1.0f : -1.0f, 0, 0}, now);
    EXPECT_TRUE(travel.Stalled());
    travel.Remember({100, 0, 0});
    EXPECT_TRUE(travel.Tried({102, 0, 0}));
    EXPECT_FALSE(travel.Tried({50, 0, 0}));
}

TEST(AllesBodyTravel, InterruptedAndOfflineTimeDoesNotConsumeTheNavigationBudget)
{
    BodyTravel travel;
    ASSERT_TRUE(travel.Commit({{0, 0, 0}, {100, 0, 0}}, 0));
    travel.Observe({0, 0, 0}, 1000);
    travel.Pause(2000);
    travel.Observe({0, 0, 0}, 100000);
    EXPECT_EQ(travel.StalledMs(), 1000u);
    travel.Observe({0, 0, 0}, 200000);
    EXPECT_EQ(travel.StalledMs(), 1000u);
    travel.Observe({0, 0, 0}, 201000);
    EXPECT_EQ(travel.StalledMs(), 2000u);
}

TEST(AllesBodyTravel, InvalidOrZeroLengthPathsCannotBeReportedAsCommittedMovement)
{
    BodyTravel travel;
    EXPECT_FALSE(travel.Commit({}, 0));
    EXPECT_FALSE(travel.Commit({{0, 0, 0}}, 0));
    EXPECT_FALSE(travel.Commit({{0, 0, 0}, {0, 0, 0}}, 0));
    EXPECT_FALSE(travel.Commit({{0, 0, 0}, {NAN, 0, 0}}, 0));
    EXPECT_FALSE(travel.HasPath());
}
