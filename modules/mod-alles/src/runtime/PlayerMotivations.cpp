/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "PlayerMotivations.h"
#include "ItemTemplate.h"
#include <algorithm>

namespace Alles
{
SatisfactionSnapshot InitialPlayerMotivations(ActorKey owner)
{
    auto state = DefaultSatisfaction();
    state.dimensions["mastery"] = {3, 0, 0, 0, MotivationCurve::Growth, 1000};
    state.dimensions["wealth"] = {2, 0, 0, 0, MotivationCurve::Growth, 1000};
    state.dimensions["equipment"] = {2, 0, 0, 0, MotivationCurve::Growth, 100};
    // Explicit uncertain priors for not-yet-known work; accepted quest rewards replace these estimates.
    state.activities["pursue_quest"]["mastery"] = 100;
    state.activities["pursue_quest"]["wealth"] = 50;
    state.activities["pursue_quest"]["equipment"] = 5;
    state.dimensions.at("security").urgency = 4;
    state.dimensions.at("rest") = {1, 1, 0, 1, MotivationCurve::Need, 1, 2};
    // Social contact complements play; it is not a rapidly recurring substitute for progression.
    state.dimensions.at("companionship") = {0.15, 0.9, 0.03, 1};
    state.activities["develop_skills"] = {{"mastery", 45}};
    // A bounded prior for a minute of uninterrupted recovery; observations, not this prediction, change health.
    state.activities.at("rest")["security"] = 0.35;
    state = PersonalizeMotivations(std::move(state), owner.id);
    state.dimensions.at("security").weight = std::max(1.5, state.dimensions.at("security").weight);
    return state;
}

bool PlayerNeedsRecovery(double health, double mana, bool continuing)
{
    return std::min(health, mana) < (continuing ? 0.9 : 0.6);
}

SatisfactionForecast ForecastPlayerActivity(uint64_t travelMs, double risk, double success, uint64_t durationMs,
    SatisfactionEffects benefits, SatisfactionEffects failures)
{
    risk = std::clamp(risk, 0.0, 1.0);
    failures["security"] = std::clamp(failures["security"] - risk * 0.4, -1.0, 1.0);
    // Movement takes time but does not consume a fictional WoW stamina resource.
    return ForecastAttempt(travelMs, durationMs, success * (1 - risk), benefits, {}, failures);
}

double EquipmentMotivationPoints(ItemTemplate const& item, uint8_t level)
{
    return item.Quality == ITEM_QUALITY_HEIRLOOM ? level * 2.33 : item.ItemLevel;
}
}
