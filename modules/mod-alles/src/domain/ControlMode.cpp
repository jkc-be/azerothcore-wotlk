/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "ControlMode.h"
#include <set>

namespace Alles
{
ControlMode ClassifyControl(ActorKey owner, std::map<ActorKey, ControlRecord> const& chain)
{
    auto own = chain.find(owner);
    if (!IsValidActor(owner) || owner.kind != ActorKind::Player || own == chain.end() || !own->second.available)
        return ControlMode::Unavailable;
    std::set<ActorKey> visited;
    auto actor = owner;
    for (unsigned depth = 0; depth < 5; ++depth)
    {
        auto found = chain.find(actor);
        if (!IsValidActor(actor) || actor.kind != ActorKind::Player || found == chain.end() || !found->second.available)
            return ControlMode::Unavailable;
        auto const& record = found->second;
        if (!visited.insert(actor).second || !record.botSession || record.selfBot || record.external)
            return ControlMode::Human;
        if (!record.ordinaryParty)
            return ControlMode::UnsupportedGroup;
        if (actor != owner && (!own->second.group || record.group != own->second.group))
            return ControlMode::Human;
        if (!record.master)
            return own->second.group ? ControlMode::AutonomousParty : ControlMode::AutonomousSolo;
        actor = *record.master;
    }
    return ControlMode::Human;
}
}
