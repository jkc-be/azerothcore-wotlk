/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "runtime/PlayerMotivations.h"
#include "ItemTemplate.h"
#include "gtest/gtest.h"

namespace Alles
{
namespace
{
SatisfactionSnapshot ComfortablePlayer()
{
    auto state = InitialPlayerMotivations({ActorKind::Player, 972});
    for (auto& [id, dimension] : state.dimensions)
        if (dimension.curve == MotivationCurve::Need)
            dimension.fulfillment = 1;
    state.dimensions.at("mastery").fulfillment = 1500;
    state.dimensions.at("wealth").fulfillment = 3500;
    state.dimensions.at("equipment").fulfillment = 30;
    return state;
}
}

TEST(AllesPlayerMotivations, UsefulQuestBeatsStayingAfterShortTermNeedsAreSatisfied)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(ComfortablePlayer()));
    auto const work = ForecastActivity(120000, 0, 0.6, 120000, {{"mastery", 250}});
    auto const before = model.Capture();
    auto const choice = model.Choose({{1, 1, work}});
    ASSERT_EQ(choice.alternatives.size(), 1u);
    EXPECT_EQ(choice.selected, 1u);
    EXPECT_GT(choice.alternatives.front().value.total, choice.staying);
    EXPECT_EQ(model.Capture(), before); // Expected rewards cannot mint XP.
    EXPECT_EQ(model.Choose({{1, 1, ForecastActivity(120000, 0, 0.6, 120000, {})}}).selected, 0u);
}

TEST(AllesPlayerMotivations, RecoveryAndLearnedDangerStillOutweighProgress)
{
    auto state = ComfortablePlayer();
    state.dimensions.at("security").fulfillment = 0.25;
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    auto const work = ForecastActivity(120000, 0, 0.6, 120000, {{"mastery", 250}});
    auto const rest = ForecastActivity(0, 0, 1, 60000, model.Effects("rest"));
    EXPECT_EQ(model.Choose({{1, 1, work}, {2, 1, rest}}).selected, 2u);
    ASSERT_TRUE(model.Restore(ComfortablePlayer()));
    ASSERT_TRUE(model.Observe(1000, 0, {}));
    for (unsigned i = 0; i < 8; ++i)
        ASSERT_TRUE(model.LearnOutcome("pursue_quest", "danger", false, 120000, {{"security", -1}}));
    auto const danger = ForecastActivity(120000, 0, model.SuccessProbability("pursue_quest", 0.6, "danger"),
        120000, {{"mastery", 250}}, model.ExpectedEffects("pursue_quest", "danger", false));
    EXPECT_EQ(model.Choose({{1, 1, danger}}).selected, 0u);
    EXPECT_EQ(model.Choose({{1, 1, danger}, {2, 1, work}}).selected, 2u);
}

TEST(AllesPlayerMotivations, StarterGearHasValueAndUpgradesCompeteWithExperience)
{
    ItemTemplate starter{};
    starter.Quality = ITEM_QUALITY_NORMAL;
    starter.ItemLevel = 1;
    ItemTemplate reward = starter;
    reward.ItemLevel = 5;
    double const gain = EquipmentMotivationPoints(reward, 3) - EquipmentMotivationPoints(starter, 3);
    EXPECT_GT(EquipmentMotivationPoints(starter, 3), 0);
    EXPECT_GT(gain, 0);
    SatisfactionModel model;
    auto state = ComfortablePlayer();
    state.dimensions.at("equipment").weight = 10;
    state.dimensions.at("mastery").weight = 0.1;
    ASSERT_TRUE(model.Restore(state));
    auto const gear = ForecastActivity(120000, 0, 0.6, 120000, {{"equipment", gain}});
    auto const experience = ForecastActivity(120000, 0, 0.6, 120000, {{"mastery", 250}});
    EXPECT_EQ(model.Choose({{1, 1, gear}, {2, 1, experience}}).selected, 1u);
    state.dimensions.at("equipment").weight = 0.1;
    state.dimensions.at("mastery").weight = 10;
    ASSERT_TRUE(model.Restore(state));
    EXPECT_EQ(model.Choose({{1, 1, gear}, {2, 1, experience}}).selected, 2u);
}

TEST(AllesPlayerMotivations, ProgressDoesNotSatiateAfterOneAchievement)
{
    SatisfactionModel model;
    auto state = ComfortablePlayer();
    state.dimensions.at("mastery").fulfillment = 100000;
    ASSERT_TRUE(model.Restore(state));
    auto const work = ForecastActivity(120000, 0, 0.6, 120000, {{"mastery", 5000}});
    EXPECT_EQ(model.Choose({{1, 1, work}}).selected, 1u);
    EXPECT_EQ(DefaultSatisfaction().dimensions.count("mastery"), 0u); // Game metrics belong to this adapter.
}
}
