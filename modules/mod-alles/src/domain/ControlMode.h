/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_CONTROL_MODE_H
#define MOD_ALLES_CONTROL_MODE_H

#include "Memory.h"
#include <map>

namespace Alles
{
enum class ControlMode : uint8_t
{
    Unavailable, Human, AutonomousSolo, AutonomousParty, UnsupportedGroup
};

struct ControlRecord
{
    bool available = false;
    bool botSession = false;
    bool selfBot = false;
    bool external = false;
    uint64_t group = 0;
    bool ordinaryParty = true;
    std::optional<ActorKey> master;
};

// Only this owner's actual master chain is supplied, not a global player roster.
ControlMode ClassifyControl(ActorKey owner, std::map<ActorKey, ControlRecord> const& chain);
}
#endif
