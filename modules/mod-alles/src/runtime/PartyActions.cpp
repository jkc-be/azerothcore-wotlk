/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "PartyActions.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestPackets.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>

namespace Alles
{
namespace
{
ActorKey Key(Player const& player)
{
    return {ActorKind::Player, player.GetGUID().GetRawValue() & 0xFFFFFFFF};
}

bool Allowed(Player& player, Cooperation const& cooperation, uint64_t now, bool ending = false)
{
    auto const mode = CurrentControlMode(player);
    return IsValidCooperation(cooperation) && cooperation.owner == Key(player)
        && cooperation.state != CooperationState::None
        && (ending || (cooperation.state != CooperationState::Cancelled
            && cooperation.state != CooperationState::Deferred
            && now >= cooperation.startedMs && now < cooperation.deadlineMs))
        && (mode == ControlMode::AutonomousSolo || mode == ControlMode::AutonomousParty)
        && player.IsAlive() && !player.IsInCombat() && !player.IsBeingTeleported();
}

bool Matches(Group const* group, Cooperation const& cooperation)
{
    if (!group || group->isRaidGroup() || group->isBGGroup() || group->isBFGroup() || group->isLFGGroup()
        || group->GetLeaderGUID() != ObjectGuid(HighGuid::Player, uint32_t(cooperation.leader.id)))
        return false;
    for (auto const& member : group->GetMemberSlots())
        if (!cooperation.agreements.contains({ActorKind::Player, member.guid.GetRawValue() & 0xFFFFFFFF}))
            return false;
    return true;
}
}

ControlMode CurrentControlMode(Player& player)
{
    std::map<ActorKey, ControlRecord> chain;
    auto* current = &player;
    for (unsigned depth = 0; current && depth < 5; ++depth)
    {
        auto const key = Key(*current);
        if (chain.contains(key))
            break;
        auto* ai = sPlayerbotsMgr.GetPlayerbotAI(current);
        auto* master = ai ? ai->GetMaster() : nullptr;
        auto const* group = current->GetGroup();
        ControlRecord record;
        record.available = current->IsInWorld() && current->GetSession() && !current->GetSession()->isLogingOut();
        record.botSession = record.available && current->GetSession()->IsBot() && ai;
        record.selfBot = IsSelfBot(current);
        record.external = ai && ai->IsExternallyControlled();
        record.group = group ? group->GetGUID().GetRawValue() : 0;
        record.ordinaryParty = !group || (!group->isRaidGroup() && !group->isBGGroup()
            && !group->isBFGroup() && !group->isLFGGroup());
        if (master)
            record.master = Key(*master);
        chain.emplace(key, record);
        current = master;
    }
    return ClassifyControl(Key(player), chain);
}

PartyObservation ObserveParty(Player& player, Cooperation const& cooperation)
{
    PartyObservation observation;
    auto const mode = CurrentControlMode(player);
    observation.ownerControlled = mode == ControlMode::Human;
    auto const* group = player.GetGroup();
    if (!group)
        return observation;
    observation.group = group->GetGUID().GetRawValue();
    if (auto const* quest = sObjectMgr->GetQuestTemplate(cooperation.quest))
        observation.requiredMembers = uint8_t(std::clamp<uint32_t>(quest->GetSuggestedPlayers(), 2, 255));
    observation.leader = {ActorKind::Player, group->GetLeaderGUID().GetRawValue() & 0xFFFFFFFF};
    observation.ordinaryParty = !group->isRaidGroup() && !group->isBGGroup()
        && !group->isBFGroup() && !group->isLFGGroup();
    auto* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    for (auto const& slot : group->GetMemberSlots())
    {
        CompanionObservation member;
        member.actor = {ActorKind::Player, slot.guid.GetRawValue() & 0xFFFFFFFF};
        auto* peer = ObjectAccessor::FindConnectedPlayer(slot.guid);
        member.online = peer && peer->IsInWorld() && peer->GetSession() && !peer->GetSession()->isLogingOut();
        if (member.online && cooperation.agreements.contains(member.actor))
        {
            member.alive = peer->IsAlive();
            member.finished = peer->GetQuestRewardStatus(cooperation.quest);
            member.readyToReward = peer->GetQuestStatus(cooperation.quest) == QUEST_STATUS_COMPLETE;
            member.eligible = member.finished || (peer->FindQuestSlot(cooperation.quest) < MAX_QUEST_LOG_SIZE
                && peer->GetQuestStatus(cooperation.quest) != QUEST_STATUS_FAILED);
            member.inRendezvous = leader && peer->IsInMap(leader) && peer->InSamePhase(leader)
                && peer->GetExactDist(leader) < 60.0f && (cooperation.state == CooperationState::Working
                    || peer->GetAreaId() == cooperation.rendezvousPlace);
            member.ready = !peer->IsBeingTeleported() && !peer->IsInFlight()
                && (peer->IsInCombat() || (peer->GetHealthPct() >= 50.0f
                    && (peer->getPowerType() != POWER_MANA || !peer->GetMaxPower(POWER_MANA)
                        || peer->GetPowerPct(POWER_MANA) >= 20.0f)));
        }
        observation.members.push_back(member);
    }
    return observation;
}

Player* FindCooperativePeer(Player& player, Cooperation const& cooperation, ActorKey peer)
{
    if (peer.kind != ActorKind::Player || !cooperation.agreements.contains(peer)
        || !Matches(player.GetGroup(), cooperation))
        return nullptr;
    auto* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32_t(peer.id)));
    return target && target->IsInWorld() && target->GetGroup() == player.GetGroup() && target->IsInMap(&player)
        && target->InSamePhase(&player) && target->IsAlive() && !target->IsBeingTeleported() ? target : nullptr;
}

bool InviteCompanion(Player& inviter, Player& companion, Cooperation const& cooperation, uint64_t now)
{
    auto* companionAI = sPlayerbotsMgr.GetPlayerbotAI(&companion);
    if (companion.GetSession() && companion.GetSession()->IsBot()
        && (!companionAI || !companionAI->rpgInfo.objectiveControl.plannerAttached))
        return false;
    if (!Allowed(inviter, cooperation, now) || cooperation.leader != Key(inviter)
        || !cooperation.agreements.contains(Key(companion)) || &inviter == &companion
        || (inviter.GetGroup() && (!Matches(inviter.GetGroup(), cooperation) || inviter.GetGroup()->IsFull())))
        return false;
    if (auto const* pending = companion.GetGroupInvite(); pending && pending->GetLeaderGUID() == inviter.GetGUID())
        return true;
    WorldPacket packet(CMSG_GROUP_INVITE, companion.GetName().size() + 5);
    packet << companion.GetName() << uint32_t(0);
    inviter.GetSession()->HandleGroupInviteOpcode(packet);
    auto const* invitation = companion.GetGroupInvite();
    return (invitation && invitation->GetLeaderGUID() == inviter.GetGUID())
        || (Matches(inviter.GetGroup(), cooperation) && companion.GetGroup() == inviter.GetGroup());
}

bool AcceptCompanionInvitation(Player& companion, Cooperation const& cooperation, uint64_t now)
{
    auto const* invitation = companion.GetGroupInvite();
    if (!Allowed(companion, cooperation, now) || cooperation.leader == Key(companion)
        || !invitation || !Matches(invitation, cooperation))
        return false;
    WorldPacket packet(CMSG_GROUP_ACCEPT, 4);
    packet << uint32_t(0);
    companion.GetSession()->HandleGroupAcceptOpcode(packet);
    return Matches(companion.GetGroup(), cooperation) && companion.GetGroup()->IsMember(companion.GetGUID());
}

bool LeaveCooperativeParty(Player& player, Cooperation const& cooperation, uint64_t now)
{
    if (!Allowed(player, cooperation, now, true) || !Matches(player.GetGroup(), cooperation))
        return false;
    WorldPacket packet(CMSG_GROUP_DISBAND, 0);
    player.GetSession()->HandleGroupDisbandOpcode(packet);
    return player.GetGroup() == nullptr;
}

std::vector<ActorKey> ShareCooperativeQuest(Player& player, Cooperation const& cooperation, uint64_t now)
{
    if (!Allowed(player, cooperation, now) || !Matches(player.GetGroup(), cooperation)
        || !player.CanShareQuest(cooperation.quest))
        return {};
    std::vector<ObjectGuid> available;
    for (auto const& slot : player.GetGroup()->GetMemberSlots())
        if (auto* member = ObjectAccessor::FindConnectedPlayer(slot.guid))
            if (member != &player && !member->GetDivider() && !member->GetQuestRewardStatus(cooperation.quest)
                && member->FindQuestSlot(cooperation.quest) >= MAX_QUEST_LOG_SIZE)
                available.push_back(slot.guid);
    if (available.empty())
        return {};
    WorldPacket packet(CMSG_PUSHQUESTTOPARTY, 4);
    packet << cooperation.quest;
    WorldPackets::Quest::PushQuestToParty request(std::move(packet));
    request.Read();
    player.GetSession()->HandlePushQuestToParty(request);
    // A real recipient divider establishes an offer; it does not establish acceptance or quest credit.
    if (!Matches(player.GetGroup(), cooperation))
        return {};
    std::vector<ActorKey> delivered;
    for (auto const guid : available)
        if (auto* member = ObjectAccessor::FindConnectedPlayer(guid))
            if (member->GetGroup() == player.GetGroup() && member->GetDivider() == player.GetGUID())
                delivered.push_back(Key(*member));
    return delivered;
}

bool AcceptSharedCooperativeQuest(Player& player, Cooperation const& cooperation, ActorKey offeredBy,
    uint32_t offeredQuest, uint64_t now)
{
    if (!Allowed(player, cooperation, now) || !Matches(player.GetGroup(), cooperation)
        || offeredQuest != cooperation.quest || offeredBy.kind != ActorKind::Player
        || player.GetDivider() != ObjectGuid(HighGuid::Player, uint32_t(offeredBy.id))
        || !player.GetDivider().IsPlayer()
        || !cooperation.agreements.contains({ActorKind::Player, player.GetDivider().GetRawValue() & 0xFFFFFFFF}))
        return false;
    WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST, 16);
    packet << player.GetDivider() << cooperation.quest << uint32_t(0);
    player.GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);
    return player.FindQuestSlot(cooperation.quest) < MAX_QUEST_LOG_SIZE;
}
}
