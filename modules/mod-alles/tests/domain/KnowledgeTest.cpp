/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Knowledge.h"
#include "gtest/gtest.h"
#include <limits>
#include <set>

namespace Alles
{
TEST(AllesKnowledge, EveryPlayableRaceHasOneOfEightVagueStartingProfiles)
{
    std::set<std::string> profiles;
    for (uint8_t race : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11})
    {
        auto const profile = StartingGeography(race);
        ASSERT_TRUE(profile);
        profiles.insert(profile->name);
        EXPECT_EQ(profile->version, 1u);
        EXPECT_GE(profile->places.size(), 3u);
        EXPECT_LE(profile->places.size(), 4u);
        for (auto const& place : profile->places)
        {
            EXPECT_NE(place.area, 0u);
            EXPECT_FALSE(place.name.empty());
            EXPECT_FALSE(place.direction.empty());
            EXPECT_EQ(place.origin, KnowledgeOrigin::Starting);
            EXPECT_EQ(place.visitedMs, 0u);
            EXPECT_EQ(place.lastUsefulWorkMs, 0u);
            EXPECT_FALSE(place.repair);
            EXPECT_LE(place.minimumLevel, place.maximumLevel);
        }
    }
    EXPECT_EQ(profiles.size(), 8u);
    EXPECT_FALSE(StartingGeography(0));
    EXPECT_FALSE(StartingGeography(9));
}

TEST(AllesKnowledge, SeedOnlyOnceAndWaitForDeathKnightStartingExperience)
{
    PrivateKnowledge knowledge;
    EXPECT_FALSE(knowledge.Seed(1, true, false));
    EXPECT_TRUE(knowledge.Places().empty());
    EXPECT_TRUE(knowledge.Seed(1, true, true));
    EXPECT_EQ(knowledge.SeedVersion(), 1u);
    EXPECT_TRUE(knowledge.Visit(9, "Northshire Valley", 1000, true));
    EXPECT_FALSE(knowledge.Seed(2, false, true));
    EXPECT_EQ(knowledge.Places().at(9).lastUsefulWorkMs, 1000u);
    EXPECT_FALSE(knowledge.Places().contains(363));
}

TEST(AllesKnowledge, ReportsRemainPrivateAttributedAndUncertain)
{
    PrivateKnowledge listener;
    PrivateKnowledge stranger;
    Reference source{ActorKey{ActorKind::Player, 42}, "Speaker"};
    auto report = listener.Hear(source, {Activity::Hunt, 0, 87, {}}, "There may be useful prey nearby", 1000, 0.4);
    ASSERT_TRUE(report);
    EXPECT_TRUE(stranger.Reports().empty());
    EXPECT_TRUE(listener.Places().empty());
    auto repeated = listener.Hear(source, {Activity::Hunt, 0, 87, {}},
        "There may be useful prey nearby", 2000, 0.6);
    EXPECT_EQ(report, repeated);
    EXPECT_EQ(listener.Reports().at(*report).receivedMs, 1000u);
    EXPECT_DOUBLE_EQ(listener.Reports().at(*report).confidence, 0.4);
    EXPECT_EQ(listener.Reports().at(*report).source, source);
    EXPECT_FALSE(listener.Hear(source, {}, "Certain hearsay", 2000, 1));
}

TEST(AllesKnowledge, TypedActivityRetrievalDoesNotRequireSharedWords)
{
    PrivateKnowledge knowledge;
    Reference source{ActorKey{ActorKind::Player, 42}, "Speaker"};
    ASSERT_TRUE(knowledge.Hear(source, {Activity::Supplies, 0, 0, {}}, "Try the settlement", 1000, 0.3));
    auto const prey = knowledge.Hear(source, {Activity::Hunt, 0, 87, {}}, "Wolves prowl nearby", 1000, 0.4);
    ASSERT_TRUE(prey);
    auto const retrieved = knowledge.Retrieve({Activity::Hunt, 0, 0, {}}, 2000);
    ASSERT_EQ(retrieved.size(), 1u);
    EXPECT_EQ(retrieved[0]->id, *prey);
    EXPECT_TRUE(knowledge.Retrieve({Activity::Hunt, 0, 0, {}}, 500).empty());
}

TEST(AllesKnowledge, AdviceAssessmentRequiresOwnNewVisitAndDoesNotRewriteTheReport)
{
    PrivateKnowledge knowledge;
    Reference source{ActorKey{ActorKind::Player, 42}, "Speaker"};
    auto const id = knowledge.Hear(source, {Activity::Work, 0, 87, {}}, "You might find work there", 1000, 0.4);
    ASSERT_TRUE(id);
    EXPECT_FALSE(knowledge.Assess(*id, 87, true));
    ASSERT_TRUE(knowledge.Visit(87, "Goldshire", 2000, false));
    EXPECT_FALSE(knowledge.Assess(*id, 87, true));
    EXPECT_TRUE(knowledge.Assess(*id, 87, false));
    EXPECT_FALSE(knowledge.Assess(*id, 87, false));
    ASSERT_TRUE(knowledge.Visit(87, "Goldshire", 3000, true));
    EXPECT_TRUE(knowledge.Assess(*id, 87, true));
    auto const& report = knowledge.Reports().at(*id);
    EXPECT_EQ(report.usefulVisits, 1u);
    EXPECT_EQ(report.unsuccessfulVisits, 1u);
    EXPECT_EQ(report.source, source);
    EXPECT_DOUBLE_EQ(report.confidence, 0.4);
    EXPECT_EQ(report.text, "You might find work there");
}

TEST(AllesKnowledge, UnvisitedReportedPlacesDoNotAcquireInventedLevelBands)
{
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Visit(999, "Observed place", 1000, false));
    EXPECT_EQ(knowledge.Places().at(999).minimumLevel, 0u);
    EXPECT_EQ(knowledge.Places().at(999).maximumLevel, 0u);
    EXPECT_TRUE(knowledge.Alternatives(9, 5).empty());
    ASSERT_TRUE(knowledge.Seed(1, false, true));
    auto const choices = knowledge.Alternatives(9, 5);
    ASSERT_EQ(choices.size(), 2u);
    EXPECT_TRUE(knowledge.Reports().empty());
}

TEST(AllesKnowledge, UsefulExperienceEstablishesOnlyTheLevelsActuallyObserved)
{
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Visit(999, "Observed place", 1000, false));
    EXPECT_TRUE(knowledge.Alternatives(9, 12).empty());
    EXPECT_FALSE(knowledge.RecordUsefulWork(999, 500, 12));
    ASSERT_TRUE(knowledge.RecordUsefulWork(999, 2000, 12));
    EXPECT_EQ(knowledge.Places().at(999).minimumLevel, 12u);
    EXPECT_EQ(knowledge.Places().at(999).maximumLevel, 12u);
    EXPECT_EQ(knowledge.Alternatives(9, 12).size(), 1u);
    EXPECT_TRUE(knowledge.Alternatives(9, 15).empty());
    ASSERT_TRUE(knowledge.RecordUsefulWork(999, 3000, 14));
    EXPECT_EQ(knowledge.Places().at(999).minimumLevel, 12u);
    EXPECT_EQ(knowledge.Places().at(999).maximumLevel, 14u);
    EXPECT_EQ(knowledge.Places().at(999).visitedMs, 1000u);
}

TEST(AllesKnowledge, RepairLocationsRequireOwnVisitAndStayPrivateAcrossRestore)
{
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Seed(1, false, true));
    RepairLocation location{0, 1, 10, 20, 30, 2000};
    EXPECT_FALSE(knowledge.RememberRepair(9, location));
    ASSERT_TRUE(knowledge.Visit(9, "Northshire Valley", 1000, false));
    location.observedMs = 500;
    EXPECT_FALSE(knowledge.RememberRepair(9, location));
    location.observedMs = 2000;
    ASSERT_TRUE(knowledge.RememberRepair(9, location));
    auto const snapshot = knowledge.Capture();
    location.observedMs = 3000;
    location.x += 1;
    EXPECT_FALSE(knowledge.RememberRepair(9, location));
    EXPECT_EQ(knowledge.Capture(), snapshot);
    PrivateKnowledge restored, stranger;
    ASSERT_TRUE(restored.Restore(snapshot));
    EXPECT_EQ(restored.Capture(), snapshot);
    EXPECT_EQ(stranger.NearestRepair(0, 1, 100, 20, 30, 3000), nullptr);
    auto const* place = restored.NearestRepair(0, 1, 100, 20, 30, 3000);
    ASSERT_NE(place, nullptr);
    EXPECT_EQ(place->area, 9u);
    EXPECT_EQ(place->repair->observedMs, 2000u);
}

TEST(AllesKnowledge, RepairReturnUsesNearestOwnMapPhaseAndBoundedDistance)
{
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Visit(9, "Valley", 1000, false));
    ASSERT_TRUE(knowledge.Visit(87, "Village", 1000, false));
    ASSERT_TRUE(knowledge.RememberRepair(9, {0, 1, 100, 0, 0, 2000}));
    ASSERT_TRUE(knowledge.RememberRepair(87, {0, 2, 200, 0, 0, 2000}));
    auto const* nearest = knowledge.NearestRepair(0, 3, 190, 0, 0, 2000);
    ASSERT_NE(nearest, nullptr);
    EXPECT_EQ(nearest->area, 87u);
    nearest = knowledge.NearestRepair(0, 1, 190, 0, 0, 2000);
    ASSERT_NE(nearest, nullptr);
    EXPECT_EQ(nearest->area, 9u);
    EXPECT_EQ(knowledge.NearestRepair(1, 1, 190, 0, 0, 2000), nullptr);
    EXPECT_EQ(knowledge.NearestRepair(0, 4, 190, 0, 0, 2000), nullptr);
    EXPECT_EQ(knowledge.NearestRepair(0, 1, 190, 0, 0, 1999), nullptr);
    EXPECT_EQ(knowledge.NearestRepair(0, 1, 701, 0, 0, 2000), nullptr);
    EXPECT_EQ(knowledge.NearestRepair(0, 1, std::numeric_limits<float>::quiet_NaN(), 0, 0, 2000), nullptr);
}

TEST(AllesKnowledge, RepairLocationsRejectUnvisitedNonfiniteAndInvalidPositions)
{
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Visit(9, "Valley", 1000, false));
    EXPECT_FALSE(knowledge.RememberRepair(9, {0, 0, 1, 2, 3, 2000}));
    EXPECT_FALSE(knowledge.RememberRepair(9, {0, 1, 1, 2, 3, 0}));
    EXPECT_FALSE(knowledge.RememberRepair(9, {0, 1, 40000, 2, 3, 2000}));
    EXPECT_FALSE(knowledge.RememberRepair(9, {0, 1, 1, 2, std::numeric_limits<float>::infinity(), 2000}));
    ASSERT_TRUE(knowledge.RememberRepair(9, {0, 1, 1, 2, 3, 2000}));
    auto snapshot = knowledge.Capture();
    snapshot.places.at(9).visitedMs = 0;
    EXPECT_FALSE(knowledge.Restore(snapshot));
}
}
