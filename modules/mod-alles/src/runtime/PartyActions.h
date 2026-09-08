/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_PARTY_ACTIONS_H
#define MOD_ALLES_PARTY_ACTIONS_H

#include "domain/ControlMode.h"
#include "domain/Cooperation.h"

class Player;

namespace Alles
{
ControlMode CurrentControlMode(Player& player);
PartyObservation ObserveParty(Player& player, Cooperation const& cooperation);
// Current ordinary-party membership permits executor-only rendezvous positioning; never retain the returned pointer.
Player* FindCooperativePeer(Player& player, Cooperation const& cooperation, ActorKey peer);
// These adapters only operate the owner's explicit agreement through normal session handlers.
bool InviteCompanion(Player& inviter, Player& companion, Cooperation const& cooperation, uint64_t now);
bool AcceptCompanionInvitation(Player& companion, Cooperation const& cooperation, uint64_t now);
bool LeaveCooperativeParty(Player& player, Cooperation const& cooperation, uint64_t now);
std::vector<ActorKey> ShareCooperativeQuest(Player& player, Cooperation const& cooperation, uint64_t now);
bool AcceptSharedCooperativeQuest(Player& player, Cooperation const& cooperation, ActorKey offeredBy,
    uint32_t offeredQuest, uint64_t now);
}
#endif
