/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Satisfaction.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <set>

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
            {
                auto const dimension = state.dimensions.find(entry.first);
                return dimension != state.dimensions.end() && Range(entry.second,
                    -MotivationLimit(dimension->second), MotivationLimit(dimension->second));
            });
}

double Response(SatisfactionDimension const& dimension, double fulfillment)
{
    if (dimension.curve == MotivationCurve::Growth)
        return std::log1p(fulfillment / dimension.scale);
    return fulfillment + dimension.satiation * fulfillment * (1 - fulfillment);
}

bool ValidExperience(SatisfactionExperience const& experience, SatisfactionSnapshot const& state)
{
    if (!experience.samples || experience.samples > 1000 || experience.successes > experience.samples
        || !Range(experience.meanDurationMs, 0, 3600000) || experience.effects.size() > state.dimensions.size())
        return false;
    for (auto const* effects : {&experience.effects, &experience.failureEffects})
        for (auto const& [id, effect] : *effects)
        {
            auto const dimension = state.dimensions.find(id);
            if (dimension == state.dimensions.end() || !effect.samples || effect.samples > 1000
                || !Range(effect.mean, -MotivationLimit(dimension->second), MotivationLimit(dimension->second)))
                return false;
        }
    return true;
}

void UpdateExperience(SatisfactionExperience& experience, bool success, uint64_t durationMs,
    SatisfactionEffects const& effects)
{
    if (experience.samples == 1000)
    {
        experience.samples /= 2;
        experience.successes /= 2;
    }
    ++experience.samples;
    experience.successes += success ? 1 : 0;
    experience.meanDurationMs += (double(durationMs) - experience.meanDurationMs) / experience.samples;
    for (auto const& [id, value] : effects)
    {
        auto& effect = (success ? experience.effects : experience.failureEffects)[id];
        if (effect.samples == 1000)
            effect.samples /= 2;
        effect.mean += (value - effect.mean) / ++effect.samples;
    }
}
}

double MotivationLimit(SatisfactionDimension const& dimension)
{
    return dimension.curve == MotivationCurve::Growth ? 1e12 : 1;
}

SatisfactionSnapshot PersonalizeMotivations(SatisfactionSnapshot snapshot, uint64_t seed)
{
    // Stable integer mixing varies each motive independently, without a fixed list of personality classes.
    for (auto& [id, dimension] : snapshot.dimensions)
    {
        uint64_t hash = seed ^ 14695981039346656037ULL;
        for (unsigned char character : id)
            hash = (hash ^ character) * 1099511628211ULL;
        hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
        hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
        hash ^= hash >> 31;
        dimension.weight = std::min(10.0, dimension.weight * (0.35 + double(hash % 10001) / 4000));
    }
    return snapshot;
}

SatisfactionSnapshot DefaultSatisfaction()
{
    SatisfactionSnapshot state{1, 0, {
        {"security", {1.5, 0.8, 0, 1}},
        {"rest", {1, 0.7, 0.6, 1}},
        {"discovery", {1, 0.4, 0.3, 1}},
        {"achievement", {1, 0.5, 0.2, 1}},
        {"companionship", {1, 0.5, 0.3, 1}},
        {"resources", {1, 0.5, 0, 1}}
    }};
    state.activities = {
        {"pursue_quest", {{"achievement", 0.3}, {"resources", 0.1}}},
        {"discover_work", {{"achievement", 0.15}}},
        {"explore_place", {{"discovery", 0.35}}},
        {"rest", {{"rest", 0.4}}},
        {"visit_companion", {{"companionship", 0.35}}},
        {"help_companion", {{"companionship", 0.2}, {"achievement", 0.1}}}
    };
    return state;
}

bool IsValidSatisfaction(SatisfactionSnapshot const& snapshot)
{
    if (!snapshot.revision || snapshot.revision >= std::numeric_limits<uint64_t>::max() - 1
        || snapshot.observedMs >= std::numeric_limits<uint64_t>::max() - 3600000
        || snapshot.dimensions.empty() || snapshot.dimensions.size() > 32 || snapshot.activities.size() > 32
        || snapshot.experiences.size() > 32 || snapshot.travel.size() > 32
        || snapshot.contexts.size() > 128 || snapshot.horizonMs < 1000 || snapshot.horizonMs > 3600000
        || snapshot.nextRestMs >= std::numeric_limits<uint64_t>::max() - 3600000
        || snapshot.nextSocialMs >= std::numeric_limits<uint64_t>::max() - 3600000)
        return false;
    double weight = 0;
    for (auto const& [id, dimension] : snapshot.dimensions)
    {
        if (!Identifier(id) || !Range(dimension.weight, 0, 10)
            || !Range(dimension.fulfillment, 0, MotivationLimit(dimension))
            || (dimension.curve != MotivationCurve::Need && dimension.curve != MotivationCurve::Growth)
            || !Range(dimension.scale, 1e-6, 1e12)
            || !Range(dimension.depletionPerHour, 0, 10) || !Range(dimension.satiation, 0, 1))
            return false;
        weight += dimension.weight;
    }
    for (auto const& [id, effects] : snapshot.activities)
        if (!Identifier(id) || !ValidEffects(effects, snapshot))
            return false;
    for (auto const& [id, experience] : snapshot.experiences)
        if (!snapshot.activities.contains(id) || !ValidExperience(experience, snapshot))
            return false;
    for (auto const& [key, experience] : snapshot.contexts)
    {
        auto const separator = key.find(':');
        if (separator == std::string::npos || !snapshot.activities.contains(key.substr(0, separator))
            || !Identifier(key.substr(separator + 1)) || !ValidExperience(experience, snapshot))
            return false;
    }
    for (auto const& [route, experience] : snapshot.travel)
        if (!Identifier(route) || !experience.samples || experience.samples > 1000
            || experience.successes > experience.samples || !Range(experience.durationRatio, 0.1, 10))
            return false;
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
            - dimension.depletionPerHour * double(activeMs) / 3600000, 0.0, MotivationLimit(dimension));
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

bool SatisfactionModel::SetActivity(std::string id, SatisfactionEffects effects)
{
    auto candidate = _state;
    candidate.activities[std::move(id)] = std::move(effects);
    ++candidate.revision;
    return Restore(std::move(candidate));
}

SatisfactionEffects SatisfactionModel::Effects(std::string const& activity, double fraction) const
{
    SatisfactionEffects result;
    auto const found = _state.activities.find(activity);
    if (found != _state.activities.end() && Range(fraction, 0, 1))
        for (auto const& [id, effect] : found->second)
            result[id] = effect * fraction;
    return result;
}

bool SatisfactionModel::Learn(std::string const& activity, bool success, uint64_t durationMs)
{
    return LearnOutcome(activity, {}, success, durationMs, {});
}

bool SatisfactionModel::LearnOutcome(std::string const& activity, std::string const& context, bool success,
    uint64_t durationMs, SatisfactionEffects const& observedEffects)
{
    if (!_state.activities.contains(activity) || durationMs > 3600000
        || (!context.empty() && !Identifier(context)) || !ValidEffects(observedEffects, _state)
        || _state.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    UpdateExperience(_state.experiences[activity], success, durationMs, observedEffects);
    if (!context.empty())
    {
        auto const key = activity + ":" + context;
        if (!_state.contexts.contains(key) && _state.contexts.size() == 128)
        {
            auto least = std::min_element(_state.contexts.begin(), _state.contexts.end(),
                [](auto const& left, auto const& right) { return left.second.samples < right.second.samples; });
            _state.contexts.erase(least);
        }
        UpdateExperience(_state.contexts[key], success, durationMs, observedEffects);
    }
    ++_state.revision;
    return true;
}

SatisfactionEffects SatisfactionModel::ExpectedEffects(std::string const& activity, std::string const& context,
    bool success) const
{
    auto effects = success ? Effects(activity) : SatisfactionEffects{};
    auto blend = [&](SatisfactionExperience const& experience)
    {
        for (auto const& [id, effect] : success ? experience.effects : experience.failureEffects)
            effects[id] = (effect.mean * effect.samples + 4 * effects[id]) / (effect.samples + 4);
    };
    if (auto const found = _state.experiences.find(activity); found != _state.experiences.end())
        blend(found->second);
    if (auto const found = _state.contexts.find(activity + ":" + context); found != _state.contexts.end())
        blend(found->second);
    return effects;
}

double SatisfactionModel::SuccessProbability(std::string const& activity, double prior) const
{
    if (!Range(prior, 0, 1))
        return 0;
    auto const found = _state.experiences.find(activity);
    if (found == _state.experiences.end())
        return prior;
    return (found->second.successes + 4 * prior) / (found->second.samples + 4);
}

uint64_t SatisfactionModel::ExpectedDuration(std::string const& activity, uint64_t priorMs) const
{
    auto const found = _state.experiences.find(activity);
    if (found == _state.experiences.end())
        return std::min(priorMs, uint64_t(3600000));
    auto const& experience = found->second;
    return uint64_t((experience.meanDurationMs * experience.samples
        + 4 * double(std::min(priorMs, uint64_t(3600000)))) / (experience.samples + 4));
}

bool SatisfactionModel::LearnTravel(std::string const& route, bool success, uint64_t observedMs, uint64_t predictedMs)
{
    if (!Identifier(route) || !observedMs || observedMs > 3600000 || !predictedMs || predictedMs > 3600000
        || _state.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    if (!_state.travel.contains(route) && _state.travel.size() == 32)
    {
        auto least = std::min_element(_state.travel.begin(), _state.travel.end(),
            [](auto const& left, auto const& right)
            { return left.second.samples < right.second.samples; });
        _state.travel.erase(least);
    }
    auto& experience = _state.travel[route];
    if (experience.samples == 1000)
    {
        experience.samples /= 2;
        experience.successes /= 2;
    }
    ++experience.samples;
    if (success)
    {
        ++experience.successes;
        double const ratio = std::clamp(double(observedMs) / double(predictedMs), 0.1, 10.0);
        experience.durationRatio += (ratio - experience.durationRatio) / experience.successes;
    }
    ++_state.revision;
    return true;
}

double SatisfactionModel::TravelSuccess(std::string const& route) const
{
    auto const found = _state.travel.find(route);
    return found == _state.travel.end() ? 1 : double(found->second.successes + 4) / (found->second.samples + 4);
}

uint64_t SatisfactionModel::TravelDuration(std::string const& route, uint64_t remainingMs) const
{
    auto const found = _state.travel.find(route);
    double const ratio = found == _state.travel.end() ? 1
        : (found->second.durationRatio * found->second.successes + 4) / (found->second.successes + 4);
    return uint64_t(std::min(3600000.0, double(remainingMs) * ratio));
}

bool SatisfactionModel::ActivityReceipt(std::string const& activity, uint64_t now)
{
    if ((activity != "rest" && activity != "visit_companion") || !now
        || now >= std::numeric_limits<uint64_t>::max() - 4200000
        || _state.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& deadline = activity == "rest" ? _state.nextRestMs : _state.nextSocialMs;
    if (now < deadline)
        return false;
    deadline = now + 600000;
    ++_state.revision;
    return true;
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
                    double const next = std::clamp(fulfillment + rate * double(step),
                        0.0, MotivationLimit(dimension));
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
                    fulfillment = std::clamp(fulfillment + effect, 0.0, MotivationLimit(dimension));
            }
            advance(remaining, 0);
            result.contributions[id] += outcome.probability * dimension.weight * integral
                / (double(horizonMs) * totalWeight);
        }
    for (auto const& [id, contribution] : result.contributions)
        result.total += contribution;
    return result;
}

SatisfactionForecast ForecastAttempt(uint64_t travelMs, uint64_t activityMs, double successProbability,
    SatisfactionEffects benefits, SatisfactionEffects travelCosts, SatisfactionEffects failureEffects)
{
    if (!Range(successProbability, 0, 1))
        return {false};
    return {true, {
        {successProbability, {{travelMs, travelCosts}, {activityMs, std::move(benefits)}}},
        {1 - successProbability, {{travelMs, travelCosts}, {activityMs, std::move(failureEffects)}}}
    }};
}

SatisfactionForecast ForecastActivity(uint64_t travelMs, double risk, double successProbability,
    uint64_t activityMs, SatisfactionEffects effects, SatisfactionEffects failureEffects)
{
    if (!Range(risk, 0, 1) || !Range(successProbability, 0, 1))
        return {false};
    double const success = successProbability * (1 - risk);
    SatisfactionEffects travelEffects{{"rest", -std::min(0.5, double(travelMs) / 3600000 * 0.2)}};
    failureEffects["security"] = std::clamp(failureEffects["security"] - risk * 0.4, -1.0, 1.0);
    return ForecastAttempt(travelMs, activityMs, success, std::move(effects), travelEffects,
        std::move(failureEffects));
}

SatisfactionDecision SatisfactionModel::Choose(std::vector<SatisfactionCandidate> const& candidates,
    uint64_t current, bool committed, double switchThreshold, SatisfactionForecast const& staying) const
{
    SatisfactionDecision result;
    result.stateRevision = _state.revision;
    auto const local = Evaluate(staying);
    if (!local)
        return result;
    result.staying = local->total;
    if (candidates.size() > 32 || !Range(switchThreshold, 0, 1))
        return result;
    std::map<uint64_t, SatisfactionAssessment> unique;
    std::set<uint64_t> seen;
    for (auto const& candidate : candidates)
    {
        if (!candidate.id || !candidate.revision || !seen.insert(candidate.id).second)
            return result;
        if (auto value = Evaluate(candidate.forecast))
            unique.emplace(candidate.id, SatisfactionAssessment{candidate.id, candidate.revision, std::move(*value)});
    }
    for (auto& [id, assessment] : unique)
        result.alternatives.push_back(std::move(assessment));
    std::stable_sort(result.alternatives.begin(), result.alternatives.end(), [](auto const& left, auto const& right)
        { return left.value.total > right.value.total; });
    auto const active = std::find_if(result.alternatives.begin(), result.alternatives.end(), [current](auto const& item)
        { return item.id == current; });
    double const best = result.alternatives.empty() ? result.staying
        : std::max(result.staying, result.alternatives.front().value.total);
    if (active != result.alternatives.end() && (committed || best <= active->value.total + switchThreshold))
        result.selected = current;
    else if (!result.alternatives.empty() && result.alternatives.front().value.total > result.staying
        + 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(result.staying)))
        result.selected = result.alternatives.front().id;
    return result;
}
}
