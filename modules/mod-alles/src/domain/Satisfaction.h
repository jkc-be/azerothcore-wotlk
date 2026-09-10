/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_SATISFACTION_H
#define MOD_ALLES_SATISFACTION_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Alles
{
// Authored simulation parameters, not a universal hierarchy of human needs. IDs are extensible data.
struct SatisfactionDimension
{
    double weight = 1;
    double fulfillment = 0.5;
    double depletionPerHour = 0;
    double satiation = 1; // 0 is linear; 1 uses 2*x - x*x, with diminishing marginal value.

    bool operator==(SatisfactionDimension const&) const = default;
};

using SatisfactionEffects = std::map<std::string, double>;

struct SatisfactionExperience
{
    uint32_t samples = 0;
    uint32_t successes = 0;
    double meanDurationMs = 0;

    bool operator==(SatisfactionExperience const&) const = default;
};

struct TravelExperience
{
    uint32_t samples = 0;
    uint32_t successes = 0;
    double durationRatio = 1; // Observed / predicted travel time, learned from successful journeys only.

    bool operator==(TravelExperience const&) const = default;
};

struct SatisfactionSnapshot
{
    uint64_t revision = 1;
    uint64_t observedMs = 0;
    std::map<std::string, SatisfactionDimension> dimensions;
    std::map<std::string, SatisfactionEffects> activities;
    std::map<std::string, SatisfactionExperience> experiences;
    uint64_t nextRestMs = 0;
    uint64_t nextSocialMs = 0;
    std::map<std::string, TravelExperience> travel;

    bool operator==(SatisfactionSnapshot const&) const = default;
};

SatisfactionSnapshot DefaultSatisfaction();
bool IsValidSatisfaction(SatisfactionSnapshot const& snapshot);

struct SatisfactionStage
{
    uint64_t durationMs = 0;
    // Net change spread across the stage; duration 0 is an event at its beginning.
    // Travel and activity costs belong here once, alongside benefits. No reward for meters walked.
    SatisfactionEffects effects;
};

struct SatisfactionOutcome
{
    double probability = 1;
    std::vector<SatisfactionStage> stages;
};

struct SatisfactionForecast
{
    bool feasible = true;
    // Mutually exclusive grounded outcomes, including failure. Probabilities must sum to one.
    // An empty stages list means maintaining the present state, subject to its ordinary depletion.
    std::vector<SatisfactionOutcome> outcomes{{}};
};

// Shared activity/route forecast. Risk is grounded in perceived threats; duration is remaining travel.
SatisfactionForecast ForecastActivity(uint64_t travelMs, double risk, double successProbability,
    uint64_t activityMs, SatisfactionEffects effects);

struct SatisfactionValue
{
    double total = 0;
    std::map<std::string, double> contributions;
};

struct SatisfactionCandidate
{
    uint64_t id = 0;
    uint64_t revision = 0;
    SatisfactionForecast forecast;
};

struct SatisfactionAssessment
{
    uint64_t id = 0;
    uint64_t revision = 0;
    SatisfactionValue value;
};

struct SatisfactionDecision
{
    uint64_t stateRevision = 0;
    uint64_t selected = 0; // Zero means maintain the present local state without a new journey.
    double staying = 0;
    std::vector<SatisfactionAssessment> alternatives;
};

class SatisfactionModel
{
public:
    SatisfactionModel() : _state(DefaultSatisfaction()) { }
    bool Restore(SatisfactionSnapshot snapshot);
    SatisfactionSnapshot const& Capture() const { return _state; }
    // One aggregate observation per game-time sample. The caller supplies only time actually observed online,
    // at most five seconds; attachment/reload starts with zero elapsed. Offline time cannot fabricate activity.
    bool Observe(uint64_t now, uint64_t activeMs, SatisfactionEffects const& effects);
    bool SetDimension(std::string id, SatisfactionDimension dimension);
    bool SetActivity(std::string id, SatisfactionEffects effects);
    SatisfactionEffects Effects(std::string const& activity, double fraction = 1) const;
    bool Learn(std::string const& activity, bool success, uint64_t durationMs);
    double SuccessProbability(std::string const& activity, double prior) const;
    uint64_t ExpectedDuration(std::string const& activity, uint64_t priorMs) const;
    bool ActivityReceipt(std::string const& activity, uint64_t now);
    bool LearnTravel(std::string const& route, bool success, uint64_t observedMs, uint64_t predictedMs);
    double TravelSuccess(std::string const& route) const;
    uint64_t TravelDuration(std::string const& route, uint64_t remainingMs) const;
    std::optional<SatisfactionValue> Evaluate(SatisfactionForecast const& forecast,
        uint64_t horizonMs = 600000) const;
    SatisfactionDecision Choose(std::vector<SatisfactionCandidate> const& candidates, uint64_t current = 0,
        bool committed = false, double switchThreshold = 0.01) const;

private:
    SatisfactionSnapshot _state;
};
}
#endif
