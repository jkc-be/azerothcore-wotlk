/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_PLANNING_WIRE_H
#define MOD_ALLES_PLANNING_WIRE_H

#include "domain/Capability.h"
#include <boost/json/object.hpp>

namespace Alles::Bridge
{
struct PlanningDecision
{
    CapabilityRequest request;
    std::string evidence;
    std::string reason;
};

// Closed version-1 response. Ownership metadata is copied from the immutable world-issued job, not the model.
PlanningDecision DecodePlanningDecision(boost::json::object const& value, CapabilityContext const& issued);
}

#endif
