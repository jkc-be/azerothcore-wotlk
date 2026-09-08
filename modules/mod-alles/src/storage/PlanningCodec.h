/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_PLANNING_CODEC_H
#define MOD_ALLES_PLANNING_CODEC_H

#include "domain/Planning.h"

namespace Alles::Storage
{
// Includes worst-case JSON escaping of all bounded strings at the domain collection limits.
constexpr std::size_t MaxPlanningBytes = 2 * 1024 * 1024;

// Versioned, owner-bound semantic state. Neither live pointers nor temporary control/movement handles are encoded.
std::string EncodePlanning(PlanningSnapshot const& snapshot);
std::optional<PlanningSnapshot> DecodePlanning(std::string_view text, ActorKey expectedOwner);
}
#endif
