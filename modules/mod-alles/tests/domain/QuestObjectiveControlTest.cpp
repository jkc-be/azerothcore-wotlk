/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "QuestObjectiveControl.h"
#include "gtest/gtest.h"

TEST(AllesQuestOwnership, StaleClaimsAndReleasesCannotReplaceAnExecutor)
{
    QuestObjectiveControl control;
    EXPECT_FALSE(control.Claim(0, 42));
    EXPECT_FALSE(control.Claim(1, 0));
    ASSERT_TRUE(control.Claim(1, 42));
    EXPECT_FALSE(control.Claim(2, 43));
    EXPECT_FALSE(control.Claim(1, 43));
    EXPECT_FALSE(control.Release(2));
    EXPECT_TRUE(control.Owns(42));
    control.Fail(QuestObjectiveControl::Failure::Navigation);
    ASSERT_TRUE(control.Claim(1, 42));
    EXPECT_EQ(control.failure, QuestObjectiveControl::Failure::Navigation);
    EXPECT_TRUE(control.Release(1));
    EXPECT_TRUE(control.retained.contains(42));
    EXPECT_FALSE(control.Owns(42));
    ASSERT_TRUE(control.Claim(2, 43));
    EXPECT_EQ(control.failure, QuestObjectiveControl::Failure::None);
}

TEST(AllesQuestOwnership, AcceptingRetryClearsDeferralButNotOtherCommitments)
{
    QuestObjectiveControl control;
    control.deferred = {42, 43};
    control.retained = {42, 43};
    ASSERT_TRUE(control.Claim(1, 42));
    EXPECT_FALSE(control.deferred.contains(42));
    EXPECT_TRUE(control.deferred.contains(43));
    EXPECT_EQ(control.retained.size(), 2u);
}

TEST(AllesQuestOwnership, PlaceAndQuestExecutorsShareOneFenceAndResetTransientSurveyEvidence)
{
    QuestObjectiveControl control;
    ASSERT_TRUE(control.ClaimPlace(1, 87));
    EXPECT_FALSE(control.Owns(0));
    EXPECT_FALSE(control.Claim(1, 42));
    EXPECT_FALSE(control.ClaimPlace(1, 9));
    EXPECT_FALSE(control.ClaimPlace(2, 87));
    control.scans = 50;
    control.lastScanEmpty = true;
    EXPECT_FALSE(control.Release(2));
    EXPECT_EQ(control.scans, 50u);
    ASSERT_TRUE(control.Release(1));
    EXPECT_EQ(control.scans, 0u);
    EXPECT_EQ(control.place, 0u);
    EXPECT_FALSE(control.lastScanEmpty);
    EXPECT_TRUE(control.Claim(2, 42));
    EXPECT_FALSE(control.ClaimPlace(2, 87));
}
