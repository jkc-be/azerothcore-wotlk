/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Capability.h"
#include <algorithm>

namespace Alles
{
bool CapabilityRegistry::Register(CapabilitySpec spec)
{
    if (_specs.size() >= 32 || spec.name.empty() || spec.name.size() > 64
        || !std::all_of(spec.name.begin(), spec.name.end(), [](unsigned char character)
        {
            return (character >= 'a' && character <= 'z') || character == '_';
        }) || spec.precondition.empty() || spec.observableEffect.empty() || spec.cancellation.empty()
        || !IsBoundedText(spec.precondition, 512) || !IsBoundedText(spec.observableEffect, 512)
        || !IsBoundedText(spec.cancellation, 512))
        return false;
    auto name = spec.name;
    return _specs.emplace(std::move(name), std::move(spec)).second;
}

std::string CapabilityRegistry::Validate(CapabilityRequest const& request, CapabilityContext const& context) const
{
    if (request.contractVersion != 1)
        return "unsupported_planning_contract";
    if (!IsValidActor(context.owner) || !context.actorGeneration || !context.objective || !context.revision
        || request.owner != context.owner || request.actorGeneration != context.actorGeneration
        || request.objective != context.objective || request.revision != context.revision)
        return "stale_objective_or_actor";
    if (!context.autonomous)
        return "control_handoff";
    auto found = _specs.find(request.capability);
    if (found == _specs.end())
        return "unavailable_capability";
    auto const& spec = found->second;
    if (spec.quest != bool(request.quest) || spec.place != bool(request.place) || spec.person != bool(request.person))
        return "invalid_parameters";
    if ((request.quest && !context.quests.contains(request.quest))
        || (request.place && !context.places.contains(request.place))
        || (request.person && !context.people.contains(*request.person)))
        return "reference_not_supplied";
    return {};
}
}
