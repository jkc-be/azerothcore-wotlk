/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "ChatAudience.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "Group.h"
#include "Player.h"
#include "SocialMgr.h"
#include "World.h"

namespace Alles
{
bool OwnsSpeechRoute(Player& speaker, SpeechRoute const& route)
{
    if (!speaker.IsInWorld())
        return false;
    if (route.type == CHAT_MSG_SAY || route.type == CHAT_MSG_YELL)
        return route.channelId == 0 && route.channelName.empty() && route.groupId == 0 && route.subgroup == 0;
    if (route.type == CHAT_MSG_CHANNEL)
    {
        auto* manager = ChannelMgr::forTeam(speaker.GetTeamId());
        auto const* channel = manager ? manager->GetChannel(route.channelName, &speaker, false) : nullptr;
        return channel && (route.channelId == 1 || route.channelId == 2)
            && channel->GetChannelId() == route.channelId && channel->GetName() == route.channelName
            && channel->HasMember(speaker.GetGUID()) && !route.groupId && !route.subgroup;
    }
    if (route.type == CHAT_MSG_PARTY)
    {
        auto* group = speaker.GetOriginalGroup();
        if (!group)
            group = speaker.GetGroup();
        return group && !group->isBGGroup() && group->GetGUID().GetRawValue() == route.groupId
            && group->GetMemberGroup(speaker.GetGUID()) == route.subgroup
            && !route.channelId && route.channelName.empty();
    }
    return false;
}

std::optional<SpeechRoute> QuestionRoute(Player& speaker)
{
    auto* group = speaker.GetOriginalGroup();
    if (!group)
        group = speaker.GetGroup();
    if (group && !group->isBGGroup() && !group->isRaidGroup() && group->GetMembersCount() > 1)
        return SpeechRoute{CHAT_MSG_PARTY, 0, "", group->GetGUID().GetRawValue(),
            group->GetMemberGroup(speaker.GetGUID())};
    if (auto* manager = ChannelMgr::forTeam(speaker.GetTeamId()))
        for (uint32_t const id : {1u, 2u})
            for (auto const& [name, channel] : manager->GetChannels())
                if (channel && channel->HasMember(speaker.GetGUID()) && channel->GetChannelId() == id
                    && channel->GetNumPlayers() > 1)
                    return SpeechRoute{CHAT_MSG_CHANNEL, id, channel->GetName()};
    auto const range = sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY);
    for (auto const& [guid, listener] : speaker.GetObjectVisibilityContainer().GetVisiblePlayersMap())
        if (listener && listener != &speaker && listener->IsInWorld() && listener->IsInMap(&speaker)
            && listener->InSamePhase(&speaker) && listener->HaveAtClient(&speaker)
            && listener->CanSeeOrDetect(&speaker) && listener->GetExactDist(&speaker) <= range)
            return SpeechRoute{CHAT_MSG_SAY};
    return std::nullopt;
}

std::optional<SpeechRoute> CaptureSpeechRoute(Player& receiver, Player& source, uint8_t type,
    std::string const& channelName)
{
    if (!receiver.IsInWorld() || !source.IsInWorld() || receiver.GetGUID() == source.GetGUID()
        || (receiver.GetSocial() && receiver.GetSocial()->HasIgnore(source.GetGUID())))
        return std::nullopt;
    SpeechRoute route;
    route.type = type;
    switch (type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
        case CHAT_MSG_WHISPER:
            return route;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        {
            auto* group = source.GetOriginalGroup();
            if (!group)
                group = source.GetGroup();
            if (!group || group->isBGGroup() || !group->IsMember(receiver.GetGUID())
                || group->GetMemberGroup(receiver.GetGUID()) != group->GetMemberGroup(source.GetGUID()))
                return std::nullopt;
            route.type = CHAT_MSG_PARTY;
            route.groupId = group->GetGUID().GetRawValue();
            route.subgroup = group->GetMemberGroup(source.GetGUID());
            return route;
        }
        case CHAT_MSG_CHANNEL:
        {
            auto* receiverChannels = ChannelMgr::forTeam(receiver.GetTeamId());
            auto* sourceChannels = ChannelMgr::forTeam(source.GetTeamId());
            auto const* channel = receiverChannels
                ? receiverChannels->GetChannel(channelName, &receiver, false) : nullptr;
            // ChatChannels.dbc identities: General=1, Trade=2. Custom/defense/LFG are not these audiences.
            if (!channel || !sourceChannels || (channel->GetChannelId() != 1 && channel->GetChannelId() != 2)
                || !channel->HasMember(receiver.GetGUID()) || !channel->HasMember(source.GetGUID())
                || sourceChannels->GetChannel(channelName, &source, false) != channel)
                return std::nullopt;
            route.channelId = channel->GetChannelId();
            route.channelName = channel->GetName();
            return route;
        }
        default:
            return std::nullopt;
    }
}
}
