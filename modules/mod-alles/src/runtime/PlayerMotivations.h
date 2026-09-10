/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_PLAYER_MOTIVATIONS_H
#define MOD_ALLES_PLAYER_MOTIVATIONS_H

#include "domain/Memory.h"
#include "domain/Satisfaction.h"

struct ItemTemplate;

namespace Alles
{
SatisfactionSnapshot InitialPlayerMotivations(ActorKey owner);
// Equipment progress includes ordinary starter items; cosmetic slots are excluded by the caller.
double EquipmentMotivationPoints(ItemTemplate const& item, uint8_t level);
}
#endif
