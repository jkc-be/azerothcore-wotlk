/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Satisfaction.h"
#include "gtest/gtest.h"
#include <limits>

namespace Alles
{
namespace
{
SatisfactionForecast Activity(uint64_t travelMs, SatisfactionEffects effects)
{
    return {true, {{1, {{travelMs, {}}, {60000, std::move(effects)}}}}};
}

double Value(SatisfactionModel const& model, SatisfactionForecast const& forecast)
{
    auto const result = model.Evaluate(forecast);
    EXPECT_TRUE(result);
    return result ? result->total : -1;
}
}

TEST(AllesSatisfaction, PreferencesAndFulfillmentChangeTheChosenActivity)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("discovery", {4, 0.1, 0, 1}));
    ASSERT_TRUE(model.SetDimension("companionship", {1, 0.1, 0, 1}));
    auto const discovery = Activity(0, {{"discovery", 0.4}});
    auto const meeting = Activity(0, {{"companionship", 0.4}});
    EXPECT_GT(Value(model, discovery), Value(model, meeting));
    ASSERT_TRUE(model.SetDimension("companionship", {8, 0.1, 0, 1}));
    EXPECT_GT(Value(model, meeting), Value(model, discovery));
    ASSERT_TRUE(model.SetDimension("companionship", {8, 1, 0, 1}));
    EXPECT_GT(Value(model, discovery), Value(model, meeting));
}

TEST(AllesSatisfaction, MultipleBenefitsAndCostsContributeWithoutAQuestReward)
{
    SatisfactionModel model;
    auto const visit = Activity(30000, {{"companionship", 0.3}, {"discovery", 0.2}, {"rest", -0.05}});
    auto const dangerousVisit = Activity(30000,
        {{"companionship", 0.3}, {"discovery", 0.2}, {"rest", -0.05}, {"security", -0.8}});
    auto const before = model.Capture();
    EXPECT_GT(Value(model, visit), Value(model, SatisfactionForecast{}));
    EXPECT_LT(Value(model, dangerousVisit), Value(model, SatisfactionForecast{}));
    EXPECT_EQ(model.Capture(), before); // Forecasts cannot award their own predicted benefit.
    auto const value = model.Evaluate(visit);
    ASSERT_TRUE(value);
    double total = 0;
    for (auto const& [id, contribution] : value->contributions)
        total += contribution;
    EXPECT_DOUBLE_EQ(value->total, total);
}

TEST(AllesSatisfaction, TravelDelayCanMakeANearbySmallerBenefitPreferable)
{
    SatisfactionModel model;
    EXPECT_GT(Value(model, Activity(10000, {{"discovery", 0.2}})),
        Value(model, Activity(540000, {{"discovery", 0.5}})));
    EXPECT_GT(Value(model, Activity(10000, {{"discovery", 0.5}})),
        Value(model, Activity(10000, {{"discovery", 0.2}})));
    auto const beyondHorizon = Activity(600000, {{"discovery", 1}});
    EXPECT_DOUBLE_EQ(Value(model, beyondHorizon), Value(model, SatisfactionForecast{}));
}

TEST(AllesSatisfaction, MaintainingFulfillmentBeatsCreatingAndRelievingTheSameDeficit)
{
    SatisfactionModel model;
    SatisfactionForecast cycle{true, {{1, {
        {60000, {{"rest", -0.5}}}, {60000, {{"rest", 0.5}}}
    }}}};
    EXPECT_GT(Value(model, SatisfactionForecast{}), Value(model, cycle));
    auto const walk = SatisfactionForecast{true, {{1, {{300000, {}}}}}};
    EXPECT_DOUBLE_EQ(Value(model, walk), Value(model, SatisfactionForecast{}));
}

TEST(AllesSatisfaction, GroundedRiskCanJustifyALongerRoute)
{
    SatisfactionModel model;
    auto const safe = Activity(120000, {{"companionship", 0.4}});
    auto dangerous = Activity(10000, {{"companionship", 0.4}});
    dangerous.outcomes[0].probability = 0.4;
    dangerous.outcomes.push_back({0.6, {{10000, {}}, {0, {{"security", -0.6}}}}});
    EXPECT_GT(Value(model, safe), Value(model, dangerous));
    // Integrate mutually exclusive outcomes separately: do not apply nonlinear satisfaction to average effects.
    auto success = dangerous;
    success.outcomes.resize(1);
    success.outcomes[0].probability = 1;
    SatisfactionForecast failure{true, {{1, dangerous.outcomes[1].stages}}};
    EXPECT_NEAR(Value(model, dangerous), 0.4 * Value(model, success) + 0.6 * Value(model, failure), 1e-12);
}

TEST(AllesSatisfaction, ObservationsAreAtomicAndCannotReplayOrInventOfflineActivity)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.Observe(1000, 0, {}));
    ASSERT_TRUE(model.Observe(2000, 1000, {{"discovery", 0.2}}));
    auto const saved = model.Capture();
    EXPECT_FALSE(model.Observe(2000, 0, {{"discovery", 0.2}}));
    EXPECT_FALSE(model.Observe(1999, 0, {{"discovery", 0.2}}));
    EXPECT_FALSE(model.Observe(3000, 1001, {{"discovery", 0.2}}));
    EXPECT_FALSE(model.Observe(3000, 1000, {{"invented", 0.2}}));
    EXPECT_EQ(model.Capture(), saved);
    SatisfactionModel loaded;
    ASSERT_TRUE(loaded.Restore(saved));
    EXPECT_FALSE(loaded.Observe(2000, 0, {{"discovery", 0.2}}));
    EXPECT_FALSE(loaded.Observe(3600000, 3598000, {}));
    ASSERT_TRUE(loaded.Observe(3600000, 0, {}));
    EXPECT_EQ(loaded.Capture().dimensions, saved.dimensions);
}

TEST(AllesSatisfaction, SupportsAnAdditionalAuthoredMotiveWithoutChangingTheEvaluator)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("craftsmanship", {3, 0.2, 0.1, 0.5}));
    EXPECT_GT(Value(model, Activity(0, {{"craftsmanship", 0.4}})), Value(model, SatisfactionForecast{}));
    auto const saved = model.Capture();
    EXPECT_FALSE(model.SetDimension("craftsmanship", {3, std::numeric_limits<double>::quiet_NaN(), 0, 1}));
    EXPECT_EQ(model.Capture(), saved);
}

TEST(AllesSatisfaction, InvalidOrUnreachableForecastsCannotWinSelection)
{
    SatisfactionModel model;
    auto forecast = Activity(1000, {{"discovery", 0.5}});
    forecast.feasible = false;
    EXPECT_FALSE(model.Evaluate(forecast));
    forecast.feasible = true;
    forecast.outcomes[0].probability = 0.5;
    EXPECT_FALSE(model.Evaluate(forecast));
    forecast.outcomes[0].probability = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(model.Evaluate(forecast));
    forecast.outcomes[0].probability = 1;
    forecast.outcomes[0].stages[1].effects["discovery"] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(model.Evaluate(forecast));
    EXPECT_FALSE(model.Evaluate(SatisfactionForecast{}, 0));
}
}
