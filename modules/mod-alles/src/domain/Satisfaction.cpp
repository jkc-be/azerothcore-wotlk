/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Satisfaction.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Alles
{
namespace
{
bool Identifier(std::string const& id)
{
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c)
        { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool Range(double value, double low, double high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

bool ValidEffects(SatisfactionEffects const& effects, SatisfactionSnapshot const& state)
{
    return effects.size() <= state.dimensions.size()
        && std::all_of(effects.begin(), effects.end(), [&](auto const& entry)
            { return state.dimensions.contains(entry.first) && Range(entry.second, -1, 1); });
}

double Response(SatisfactionDimension const& dimension, double fulfillment)
{
    return fulfillment + dimension.satiation * fulfillment * (1 - fulfillment);
}
}

SatisfactionSnapshot DefaultSatisfaction()
{
    return {1, 0, {
        {"security", {1.5, 0.8, 0, 1}},
        {"rest", {1, 0.7, 0.6, 1}},
        {"discovery", {1, 0.4, 0.3, 1}},
        {"achievement", {1, 0.5, 0.2, 1}},
        {"companionship", {1, 0.5, 0.3, 1}},
        {"resources", {1, 0.5, 0, 1}}
    }};
}

bool IsValidSatisfaction(SatisfactionSnapshot const& snapshot)
{
    if (!snapshot.revision || snapshot.revision >= std::numeric_limits<uint64_t>::max() - 1
        || snapshot.observedMs >= std::numeric_limits<uint64_t>::max() - 3600000
        || snapshot.dimensions.empty() || snapshot.dimensions.size() > 32)
        return false;
    double weight = 0;
    for (auto const& [id, dimension] : snapshot.dimensions)
    {
        if (!Identifier(id) || !Range(dimension.weight, 0, 10) || !Range(dimension.fulfillment, 0, 1)
            || !Range(dimension.depletionPerHour, 0, 10) || !Range(dimension.satiation, 0, 1))
            return false;
        weight += dimension.weight;
    }
    return weight > 0;
}

bool SatisfactionModel::Restore(SatisfactionSnapshot snapshot)
{
    if (!IsValidSatisfaction(snapshot))
        return false;
    _state = std::move(snapshot);
    return true;
}

bool SatisfactionModel::Observe(uint64_t now, uint64_t activeMs, SatisfactionEffects const& effects)
{
    if (!now || now <= _state.observedMs || activeMs > 5000 || activeMs > now - _state.observedMs
        || (!_state.observedMs && activeMs) || !ValidEffects(effects, _state)
        || _state.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    for (auto& [id, dimension] : _state.dimensions)
    {
        auto const found = effects.find(id);
        double const effect = found == effects.end() ? 0 : found->second;
        dimension.fulfillment = std::clamp(dimension.fulfillment + effect
            - dimension.depletionPerHour * double(activeMs) / 3600000, 0.0, 1.0);
    }
    _state.observedMs = now;
    ++_state.revision;
    return true;
}

bool SatisfactionModel::SetDimension(std::string id, SatisfactionDimension dimension)
{
    auto candidate = _state;
    candidate.dimensions[std::move(id)] = dimension;
    ++candidate.revision;
    return Restore(std::move(candidate));
}

std::optional<SatisfactionValue> SatisfactionModel::Evaluate(SatisfactionForecast const& forecast,
    uint64_t horizonMs) const
{
    if (!forecast.feasible || horizonMs < 1000 || horizonMs > 3600000
        || forecast.outcomes.empty() || forecast.outcomes.size() > 8)
        return std::nullopt;
    double probability = 0;
    for (auto const& outcome : forecast.outcomes)
    {
        if (!Range(outcome.probability, 0, 1) || outcome.stages.size() > 16)
            return std::nullopt;
        probability += outcome.probability;
        for (auto const& stage : outcome.stages)
            if (stage.durationMs > 3600000 || !ValidEffects(stage.effects, _state))
                return std::nullopt;
    }
    if (std::abs(probability - 1) > 1e-9)
        return std::nullopt;
    double totalWeight = 0;
    for (auto const& [id, dimension] : _state.dimensions)
        totalWeight += dimension.weight;
    SatisfactionValue result;
    for (auto const& outcome : forecast.outcomes)
        for (auto const& [id, dimension] : _state.dimensions)
        {
            double fulfillment = dimension.fulfillment;
            double integral = 0;
            uint64_t remaining = horizonMs;
            auto advance = [&](uint64_t duration, double effect)
            {
                double const rate = (duration ? effect / double(duration) : 0)
                    - dimension.depletionPerHour / 3600000;
                uint64_t elapsed = std::min(duration, remaining);
                remaining -= elapsed;
                while (elapsed)
                {
                    auto const step = std::min(elapsed, uint64_t(5000));
                    double const next = std::clamp(fulfillment + rate * double(step), 0.0, 1.0);
                    // Bounded numerical integration of satisfaction held over time, rather than positive gains.
                    integral += (Response(dimension, fulfillment) + Response(dimension, next)) * double(step) / 2;
                    fulfillment = next;
                    elapsed -= step;
                }
            };
            for (auto const& stage : outcome.stages)
            {
                if (!remaining)
                    break;
                auto const found = stage.effects.find(id);
                double const effect = found == stage.effects.end() ? 0 : found->second;
                if (stage.durationMs)
                    advance(stage.durationMs, effect);
                else
                    fulfillment = std::clamp(fulfillment + effect, 0.0, 1.0);
            }
            advance(remaining, 0);
            result.contributions[id] += outcome.probability * dimension.weight * integral
                / (double(horizonMs) * totalWeight);
        }
    for (auto const& [id, contribution] : result.contributions)
        result.total += contribution;
    return result;
}
}
