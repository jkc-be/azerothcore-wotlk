/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_PLANNING_H
#define MOD_ALLES_PLANNING_H

#include "Knowledge.h"
#include "Objective.h"
#include "Satisfaction.h"
#include <limits>
#include <algorithm>

namespace Alles
{
struct PlanningSnapshot
{
    ActorKey owner;
    uint64_t revision = 0;
    ObjectiveSnapshot objectives;
    KnowledgeSnapshot knowledge;
    SatisfactionSnapshot satisfaction = DefaultSatisfaction();

    bool operator==(PlanningSnapshot const&) const = default;
};

inline bool IsValidPlanningSnapshot(PlanningSnapshot const& snapshot)
{
    return IsValidActor(snapshot.owner) && snapshot.revision
        && snapshot.revision < std::numeric_limits<uint64_t>::max()
        && IsValidObjectiveSnapshot(snapshot.objectives) && IsValidKnowledgeSnapshot(snapshot.knowledge)
        && IsValidSatisfaction(snapshot.satisfaction)
        && !snapshot.knowledge.contacts.contains(snapshot.owner)
        && std::all_of(snapshot.objectives.objectives.begin(), snapshot.objectives.objectives.end(),
            [&](auto const& item)
            {
                if (item.second.request && item.second.request->source.actor == snapshot.owner)
                    return false;
                return item.second.cooperation.state == CooperationState::None
                    || (item.second.cooperation.owner == snapshot.owner
                        && snapshot.knowledge.places.contains(item.second.cooperation.rendezvousPlace));
            });
}
}
#endif
