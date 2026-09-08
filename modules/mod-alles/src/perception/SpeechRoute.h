/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_SPEECH_ROUTE_H
#define MOD_ALLES_SPEECH_ROUTE_H

#include <cstdint>
#include <string>
#include <string_view>

namespace Alles
{
// Delivery-time values, never a retained Channel/Group pointer or a client's numbered channel slot.
struct SpeechRoute
{
    uint8_t type = 0;
    uint32_t channelId = 0;
    std::string channelName;
    uint64_t groupId = 0;
    uint8_t subgroup = 0;

    bool operator==(SpeechRoute const&) const = default;
};

bool IsSafeChatText(std::string_view text);
bool IsRemoteSpeech(uint8_t type);
std::string SpeechRouteLabel(SpeechRoute const& route);
}

#endif
