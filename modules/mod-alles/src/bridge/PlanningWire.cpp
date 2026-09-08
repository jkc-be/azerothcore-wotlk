/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "PlanningWire.h"
#include "Wire.h"
#include <limits>
#include <stdexcept>

namespace Alles::Bridge
{
PlanningDecision DecodePlanningDecision(boost::json::object const& value, CapabilityContext const& issued)
{
    Fields(value, {"version", "capability", "quest", "place", "person", "evidence", "reason"});
    if (Number(value, "version") != 1 || Number(value, "quest") > std::numeric_limits<uint32_t>::max()
        || Number(value, "place") > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("unsupported planning response");
    PlanningDecision decision;
    auto& request = decision.request;
    request.owner = issued.owner;
    request.actorGeneration = issued.actorGeneration;
    request.objective = issued.objective;
    request.revision = issued.revision;
    request.capability = String(value, "capability", 64);
    request.quest = uint32_t(Number(value, "quest"));
    request.place = uint32_t(Number(value, "place"));
    auto const& person = value.at("person");
    if (!person.is_null())
    {
        auto const& object = person.as_object();
        Fields(object, {"kind", "id"});
        if (Number(object, "kind") > uint8_t(ActorKind::CreatureSpawn))
            throw std::invalid_argument("invalid planning actor");
        request.person = ActorKey{ActorKind(Number(object, "kind")), Number(object, "id")};
        if (!IsValidActor(*request.person))
            throw std::invalid_argument("invalid planning actor");
    }
    decision.evidence = String(value, "evidence", 64);
    decision.reason = String(value, "reason", 512);
    if (request.capability.empty() || !IsBoundedText(request.capability, 64)
        || !IsBoundedText(decision.evidence, 64) || !IsBoundedText(decision.reason, 512))
        throw std::invalid_argument("invalid planning text");
    return decision;
}
}
