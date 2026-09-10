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

TEST(AllesSatisfaction, StayingFacesTheSameObservedExposureAsALocalActivity)
{
    SatisfactionModel model;
    auto const exposed = ForecastActivity(0, 0.8, 1, 60000, {});
    auto const safe = model.Choose({});
    auto const decision = model.Choose({{1, 1, exposed}}, 0, false, 0.01, exposed);
    ASSERT_EQ(decision.alternatives.size(), 1u);
    EXPECT_DOUBLE_EQ(decision.staying, decision.alternatives.front().value.total);
    EXPECT_LT(decision.staying, safe.staying);
    EXPECT_EQ(decision.selected, 0u); // An equally exposed activity offers no invented benefit.
}

TEST(AllesSatisfaction, ObservedLocalDangerCanMakeLeavingWorthItsTravelCost)
{
    SatisfactionModel model;
    auto const before = model.Capture();
    auto const walk = ForecastActivity(30000, 0, 1, 60000, {});
    std::vector<SatisfactionCandidate> const alternatives{{1, 1, walk}};
    EXPECT_EQ(model.Choose(alternatives).selected, 0u); // Safe and fulfilled: unnecessary travel loses.
    auto const exposed = ForecastActivity(0, 0.8, 1, 60000, {});
    EXPECT_EQ(model.Choose(alternatives, 0, false, 0.01, exposed).selected, 1u);
    EXPECT_EQ(model.Capture(), before); // Reconsideration cannot turn a prediction into observed damage.
    EXPECT_EQ(model.Choose(alternatives, 0, false, 0.01, SatisfactionForecast{false}).selected, 0u);
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

TEST(AllesSatisfaction, SelectionUsesSharedForecastsAndHoldsOnlyFeasibleCommitments)
{
    SatisfactionModel model;
    auto slow = Activity(540000, {{"discovery", 0.5}});
    auto near = Activity(10000, {{"discovery", 0.3}});
    auto choice = model.Choose({{1, 1, slow}, {2, 1, near}});
    EXPECT_EQ(choice.selected, 2u);
    ASSERT_EQ(choice.alternatives.size(), 2u);
    EXPECT_EQ(choice.alternatives.front().id, 2u);
    EXPECT_EQ(model.Choose({{1, 1, slow}, {2, 1, near}}, 1, true).selected, 1u);
    EXPECT_EQ(model.Choose({{1, 1, slow}, {2, 1, near}}, 1, false, 0).selected, 2u);
    slow.feasible = false;
    EXPECT_EQ(model.Choose({{1, 1, slow}, {2, 1, near}}, 1, true).selected, 2u);
    EXPECT_EQ(model.Choose({{1, 1, Activity(0, {{"security", -0.5}})}}).selected, 0u);
}

TEST(AllesSatisfaction, TinyImprovementsDoNotAlternateDestinations)
{
    SatisfactionModel model;
    auto first = Activity(10000, {{"discovery", 0.3}});
    auto second = Activity(9999, {{"discovery", 0.3}});
    EXPECT_EQ(model.Choose({{1, 1, first}, {2, 1, second}}, 1).selected, 1u);
    EXPECT_EQ(model.Choose({{1, 1, first}, {2, 1, second}}, 2).selected, 2u);
}

TEST(AllesSatisfaction, ObservedOutcomesChangeExpectationsAndPersistWithoutAwardingFulfillment)
{
    SatisfactionModel model;
    auto const fulfillment = model.Capture().dimensions;
    auto const prior = model.SuccessProbability("explore_place", 0.8);
    ASSERT_TRUE(model.Learn("explore_place", false, 240000));
    EXPECT_LT(model.SuccessProbability("explore_place", 0.8), prior);
    EXPECT_GT(model.ExpectedDuration("explore_place", 60000), 60000u);
    EXPECT_EQ(model.Capture().dimensions, fulfillment);
    SatisfactionModel loaded;
    ASSERT_TRUE(loaded.Restore(model.Capture()));
    EXPECT_EQ(loaded.SuccessProbability("explore_place", 0.8), model.SuccessProbability("explore_place", 0.8));
    EXPECT_FALSE(loaded.Learn("unknown_activity", true, 1000));
    EXPECT_FALSE(loaded.Learn("explore_place", true, 3600001));
}

TEST(AllesSatisfaction, AuthoredActivitiesCanFulfillAdditionalMotives)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("generosity", {3, 0.1, 0.2, 1}));
    ASSERT_TRUE(model.SetActivity("visit_companion", {{"generosity", 0.4}, {"companionship", 0.2}}));
    auto const effects = model.Effects("visit_companion", 0.5);
    EXPECT_DOUBLE_EQ(effects.at("generosity"), 0.2);
    EXPECT_DOUBLE_EQ(effects.at("companionship"), 0.1);
    EXPECT_GT(Value(model, Activity(0, effects)), Value(model, SatisfactionForecast{}));
    EXPECT_FALSE(model.SetActivity("visit_companion", {{"unknown_motive", 0.2}}));
}

TEST(AllesSatisfaction, ActivityCooldownSurvivesLossOfAnEvictableIntentionAndReload)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.ActivityReceipt("visit_companion", 1000));
    ASSERT_TRUE(model.ActivityReceipt("rest", 1000));
    SatisfactionModel loaded;
    ASSERT_TRUE(loaded.Restore(model.Capture()));
    EXPECT_FALSE(loaded.ActivityReceipt("visit_companion", 2000));
    EXPECT_FALSE(loaded.ActivityReceipt("rest", 600999));
    EXPECT_TRUE(loaded.ActivityReceipt("rest", 601000));
    EXPECT_TRUE(loaded.ActivityReceipt("visit_companion", 601000));
}
TEST(AllesSatisfaction, SharedRouteForecastTradesArrivalDelayAgainstPerceivedRisk)
{
    SatisfactionModel model;
    auto const effects = model.Effects("visit_companion");
    auto const direct = ForecastActivity(10000, 0.6, 0.9, 10000, effects);
    auto const detour = ForecastActivity(90000, 0.05, 0.9, 10000, effects);
    EXPECT_GT(Value(model, detour), Value(model, direct));
    EXPECT_LT(Value(model, ForecastActivity(1200000, 0, 0.9, 10000, effects)),
        Value(model, SatisfactionForecast{}));
    EXPECT_FALSE(model.Evaluate(ForecastActivity(0, -0.1, 1, 60000, effects)));
    EXPECT_FALSE(model.Evaluate(ForecastActivity(0, 0, 2, 60000, effects)));
    auto const original = model.Capture();
    for (unsigned index = 0; index < 20; ++index)
        EXPECT_EQ(model.Choose({{1, 1, direct}, {2, 1, detour}}).selected, 2u);
    EXPECT_EQ(model.Capture(), original);
}

TEST(AllesSatisfaction, DuplicateCandidatesAreRejectedEvenIfTheFirstIsInfeasible)
{
    SatisfactionModel model;
    auto const decision = model.Choose({{1, 1, {false}}, {1, 2, Activity(0, {{"discovery", 1}})}});
    EXPECT_EQ(decision.selected, 0u);
    EXPECT_TRUE(decision.alternatives.empty());
}

TEST(AllesSatisfaction, RouteOutcomesChangeExpectationsWithoutAwardingFulfillment)
{
    SatisfactionModel model;
    auto const fulfillment = model.Capture().dimensions;
    EXPECT_DOUBLE_EQ(model.TravelSuccess("route_a"), 1);
    EXPECT_EQ(model.TravelDuration("route_a", 10000), 10000u);
    ASSERT_TRUE(model.LearnTravel("route_a", false, 30000, 10000));
    EXPECT_LT(model.TravelSuccess("route_a"), 1);
    EXPECT_EQ(model.TravelDuration("route_a", 10000), 10000u); // A failed trip does not establish arrival speed.
    EXPECT_DOUBLE_EQ(model.TravelSuccess("route_b"), 1); // Other approaches remain independent.
    ASSERT_TRUE(model.LearnTravel("route_a", true, 20000, 10000));
    EXPECT_GT(model.TravelDuration("route_a", 10000), 10000u);
    EXPECT_EQ(model.TravelDuration("route_a", 0), 0u); // Sunk distance is never charged again.
    EXPECT_EQ(model.Capture().dimensions, fulfillment);
    auto const saved = model.Capture();
    ASSERT_TRUE(model.Restore(saved));
    EXPECT_EQ(model.Capture(), saved);
    EXPECT_FALSE(model.LearnTravel("route_a", true, 0, 10000));
    EXPECT_FALSE(model.LearnTravel("route_a", true, 10000, 0));
    for (unsigned index = 0; index < 50; ++index)
        EXPECT_TRUE(model.LearnTravel("route_" + std::to_string(index), false, 1000, 1000));
    EXPECT_EQ(model.Capture().travel.size(), 32u);
    EXPECT_TRUE(IsValidSatisfaction(model.Capture()));
}

TEST(AllesSatisfaction, ContinuingAmbitionsValueFurtherProgressAndAccountForLoss)
{
    SatisfactionSnapshot state;
    state.dimensions = {{"wealth", {1, 1000000, 0, 0, MotivationCurve::Growth, 1000}}};
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    auto const before = model.Capture();
    auto const earn = ForecastAttempt(0, 60000, 1, {{"wealth", 10000}});
    EXPECT_GT(Value(model, earn), Value(model, {}));
    EXPECT_EQ(model.Choose({{1, 1, earn}}).selected, 1u);
    EXPECT_LT(Value(model, ForecastAttempt(0, 60000, 1, {{"wealth", -10000}})), Value(model, {}));
    SatisfactionForecast loseThenRecover{true, {{1, {{0, {{"wealth", -10000}}},
        {30000, {}}, {0, {{"wealth", 10000}}}}}}};
    EXPECT_LT(Value(model, loseThenRecover), Value(model, {}));
    EXPECT_EQ(model.Capture(), before);
    ASSERT_TRUE(model.Observe(1000, 0, {{"wealth", 10000}}));
    EXPECT_DOUBLE_EQ(model.Capture().dimensions.at("wealth").fulfillment, 1010000);
    state.dimensions.at("wealth").fulfillment = 1e9;
    ASSERT_TRUE(model.Restore(state));
    EXPECT_EQ(model.Choose({{1, 1, ForecastAttempt(0, 60000, 1, {{"wealth", 10}})}}).selected, 1u);
}

TEST(AllesSatisfaction, AnotherScenarioUsesItsOwnMetricsActionsAndPersonalPriorities)
{
    SatisfactionSnapshot state;
    state.dimensions = {
        {"credits", {4, 10, 0, 0, MotivationCurve::Growth, 10}},
        {"knowledge", {1, 10, 0, 0, MotivationCurve::Growth, 10}},
        {"energy", {1, 0.8, 0.1, 1}}
    };
    state.activities = {{"sell_crop", {{"credits", 20}}}, {"research", {{"knowledge", 20}}}};
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    std::vector<SatisfactionCandidate> actions{
        {1, 1, ForecastAttempt(10000, 60000, 1, model.Effects("sell_crop"), {{"energy", -0.1}})},
        {2, 1, ForecastAttempt(10000, 60000, 1, model.Effects("research"), {{"energy", -0.1}})}};
    EXPECT_EQ(model.Choose(actions).selected, 1u);
    state.dimensions.at("credits").weight = 1;
    state.dimensions.at("knowledge").weight = 4;
    ASSERT_TRUE(model.Restore(state));
    EXPECT_EQ(model.Choose(actions).selected, 2u);
    // No WoW need names, item IDs, quests, actor classes or navigation APIs are needed by the model.
    EXPECT_EQ(model.Capture().dimensions.size(), 3u);
}

TEST(AllesSatisfaction, OutcomeLearningChangesChoicesAndTransfersToUnseenContexts)
{
    SatisfactionSnapshot state;
    state.dimensions = {{"credits", {1, 10, 0, 0, MotivationCurve::Growth, 10}}};
    state.activities = {{"harvest", {{"credits", 5}}}, {"trade", {{"credits", 20}}}};
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    auto choose = [&]()
    {
        return model.Choose({
            {1, 1, ForecastAttempt(0, 60000, 1, model.ExpectedEffects("harvest", "field"))},
            {2, 1, ForecastAttempt(0, 60000, 1, model.ExpectedEffects("trade", "market"))}}).selected;
    };
    EXPECT_EQ(choose(), 2u);
    for (unsigned sample = 0; sample < 20; ++sample)
        ASSERT_TRUE(model.LearnOutcome("harvest", "field", true, 60000, {{"credits", 50}}));
    EXPECT_EQ(choose(), 1u);
    EXPECT_GT(model.ExpectedEffects("harvest", "new_field").at("credits"), 20);
    EXPECT_GT(model.ExpectedEffects("harvest", "field").at("credits"),
        model.ExpectedEffects("harvest", "new_field").at("credits"));
    for (unsigned sample = 0; sample < 100; ++sample)
        ASSERT_TRUE(model.LearnOutcome("harvest", "field", true, 60000, {{"credits", 0}}));
    EXPECT_EQ(choose(), 2u); // Repeated zero yield corrects the old optimistic belief.
    EXPECT_EQ(model.Capture().dimensions, state.dimensions); // Knowledge changes, possessions do not.
}

TEST(AllesSatisfaction, FailedOutcomesLearnConsequencesSeparatelyFromSuccessfulBenefits)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.SetDimension("wealth", {2, 100, 0, 0, MotivationCurve::Growth, 100}));
    ASSERT_TRUE(model.SetActivity("experiment", {{"wealth", 50}}));
    for (unsigned sample = 0; sample < 20; ++sample)
        ASSERT_TRUE(model.LearnOutcome("experiment", "lab", false, 10000, {{"wealth", -10}}));
    EXPECT_DOUBLE_EQ(model.ExpectedEffects("experiment", "lab").at("wealth"), 50);
    EXPECT_LT(model.ExpectedEffects("experiment", "lab", false).at("wealth"), -8);
    EXPECT_LT(model.SuccessProbability("experiment", 0.5), 0.1);
    auto const saved = model.Capture();
    EXPECT_FALSE(model.LearnOutcome("experiment", "lab", true, 10000,
        {{"wealth", std::numeric_limits<double>::quiet_NaN()}}));
    EXPECT_FALSE(model.LearnOutcome("experiment", "invalid:context", true, 10000, {}));
    EXPECT_EQ(model.Capture(), saved);
}

TEST(AllesSatisfaction, IndividualPrioritiesAreStableAndPlanningHorizonIsAuthored)
{
    auto state = DefaultSatisfaction();
    state.dimensions.at("rest").weight = 0;
    EXPECT_EQ(PersonalizeMotivations(state, 42), PersonalizeMotivations(state, 42));
    EXPECT_NE(PersonalizeMotivations(state, 42).dimensions, PersonalizeMotivations(state, 43).dimensions);
    EXPECT_DOUBLE_EQ(PersonalizeMotivations(state, 42).dimensions.at("rest").weight, 0);
    state.dimensions = {{"knowledge", {1, 10, 0, 0, MotivationCurve::Growth, 10}}};
    state.activities.clear();
    state.horizonMs = 60000;
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    auto const delayed = ForecastAttempt(120000, 60000, 1, {{"knowledge", 20}});
    EXPECT_EQ(model.Choose({{1, 1, delayed}}).selected, 0u);
    state.horizonMs = 600000;
    ASSERT_TRUE(model.Restore(state));
    EXPECT_EQ(model.Choose({{1, 1, delayed}}).selected, 1u);
    state.horizonMs = 0;
    EXPECT_FALSE(model.Restore(state));
}

TEST(AllesSatisfaction, InvalidCurvesScalesAndBoundedLearningCannotCorruptState)
{
    SatisfactionModel model;
    auto const before = model.Capture();
    EXPECT_FALSE(model.SetDimension("wealth", {1, 10, 0, 0, MotivationCurve::Growth, 0}));
    EXPECT_FALSE(model.SetDimension("wealth", {1, 10, 0, 0, MotivationCurve(9), 1}));
    EXPECT_FALSE(model.SetDimension("wealth", {1, -1, 0, 0, MotivationCurve::Growth, 1}));
    EXPECT_EQ(model.Capture(), before);
    for (unsigned index = 0; index < 150; ++index)
        ASSERT_TRUE(model.LearnOutcome("rest", "site_" + std::to_string(index), true, 60000, {{"rest", 0.2}}));
    EXPECT_EQ(model.Capture().contexts.size(), 128u);
    EXPECT_TRUE(IsValidSatisfaction(model.Capture()));
}

TEST(AllesSatisfaction, SurvivalUrgencyYieldsToPersonalAmbitionAfterRecovery)
{
    SatisfactionSnapshot state;
    state.dimensions = {
        {"vitality", {1, 0.2, 0, 1, MotivationCurve::Need, 1, 4}},
        {"wealth", {4, 100, 0, 0, MotivationCurve::Growth, 100}}
    };
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    auto const heal = ForecastAttempt(0, 60000, 1, {{"vitality", 0.5}});
    auto const work = ForecastAttempt(0, 60000, 1, {{"wealth", 50}});
    std::vector<SatisfactionCandidate> choices{{1, 1, heal}, {2, 1, work}};
    EXPECT_EQ(model.Choose(choices).selected, 1u);
    auto const weights = model.Capture().dimensions;
    ASSERT_TRUE(model.Observe(1000, 0, {{"vitality", 0.8}}));
    EXPECT_EQ(model.Choose(choices).selected, 2u);
    EXPECT_EQ(model.Capture().dimensions.at("wealth"), weights.at("wealth"));
    EXPECT_EQ(model.Capture().dimensions.at("vitality").weight, weights.at("vitality").weight);
}

TEST(AllesSatisfaction, InjuryMakesFurtherHarmCostlierAndKeepsSaferOpportunitiesValuable)
{
    SatisfactionSnapshot state;
    state.dimensions = {
        {"vitality", {2, 0.8, 0, 1, MotivationCurve::Need, 1, 4}},
        {"discovery", {1, 0.3, 0, 1}}
    };
    SatisfactionModel model;
    ASSERT_TRUE(model.Restore(state));
    auto const harm = ForecastAttempt(0, 60000, 1, {{"vitality", -0.1}});
    double const healthyLoss = Value(model, {}) - Value(model, harm);
    state.dimensions.at("vitality").fulfillment = 0.3;
    ASSERT_TRUE(model.Restore(state));
    EXPECT_GT(Value(model, {}) - Value(model, harm), healthyLoss);
    auto const safe = ForecastAttempt(0, 60000, 1, {{"discovery", 0.2}});
    auto const reckless = ForecastAttempt(0, 60000, 0.5, {{"discovery", 0.6}}, {}, {{"vitality", -0.3}});
    EXPECT_EQ(model.Choose({{1, 1, safe}, {2, 1, reckless}}).selected, 1u);
    EXPECT_GT(Value(model, safe), Value(model, {})); // Survival pressure does not require permanent inaction.
    EXPECT_FALSE(model.SetDimension("vitality", {2, 0.3, 0, 1, MotivationCurve::Need, 1, -1}));
    EXPECT_FALSE(model.SetDimension("wealth", {2, 10, 0, 0, MotivationCurve::Growth, 1, 4}));
}

TEST(AllesSatisfaction, PersonalHarmChangesOnlyItsContextAndSurvivesRecoveryWithRevisableEvidence)
{
    SatisfactionModel model;
    ASSERT_TRUE(model.Observe(1000, 0, {}));
    auto value = [&](std::string const& context)
    {
        auto forecast = ForecastAttempt(10000, 60000, model.SuccessProbability("explore_place", 0.85, context),
            model.ExpectedEffects("explore_place", context), {},
            model.ExpectedEffects("explore_place", context, false));
        return Value(model, forecast);
    };
    double const safe = value("safe");
    for (unsigned i = 0; i < 4; ++i)
        ASSERT_TRUE(model.LearnOutcome("explore_place", "danger", false, 30000, {{"security", -1}}));
    EXPECT_LT(value("danger"), safe);
    EXPECT_DOUBLE_EQ(value("safe"), safe);
    double const harmed = model.SuccessProbability("explore_place", 0.85, "danger");
    ASSERT_TRUE(model.Observe(2000, 1000, {{"security", -1}}));
    ASSERT_TRUE(model.Observe(3000, 0, {{"security", 1}})); // Actual recovery does not erase learned outcomes.
    SatisfactionModel restored;
    ASSERT_TRUE(restored.Restore(model.Capture()));
    EXPECT_NEAR(restored.SuccessProbability("explore_place", 0.85, "danger"), harmed, 0.001);
    for (unsigned i = 0; i < 8; ++i)
        ASSERT_TRUE(model.LearnOutcome("explore_place", "danger", true, 60000, {{"security", 0}}));
    EXPECT_GT(model.SuccessProbability("explore_place", 0.85, "danger"), harmed);
    ASSERT_TRUE(restored.Observe(216000000, 0, {}));
    EXPECT_GT(restored.SuccessProbability("explore_place", 0.85, "danger"), harmed);
    EXPECT_NEAR(restored.SuccessProbability("explore_place", 0.85, "danger"), 0.85, 0.002);
}

}
