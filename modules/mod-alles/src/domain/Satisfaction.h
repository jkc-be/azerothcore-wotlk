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

struct SatisfactionSnapshot
{
    uint64_t revision = 1;
    uint64_t observedMs = 0;
    std::map<std::string, SatisfactionDimension> dimensions;

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

struct SatisfactionValue
{
    double total = 0;
    std::map<std::string, double> contributions;
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
    std::optional<SatisfactionValue> Evaluate(SatisfactionForecast const& forecast,
        uint64_t horizonMs = 600000) const;

private:
    SatisfactionSnapshot _state;
};
}
#endif
