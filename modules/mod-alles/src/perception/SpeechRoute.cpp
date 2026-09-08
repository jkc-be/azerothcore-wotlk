/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "SpeechRoute.h"
#include "SharedDefines.h"
#include "domain/Memory.h"
#include <algorithm>

namespace Alles
{
bool IsSafeChatText(std::string_view text)
{
    if (text.empty() || text.size() > 255 || !IsBoundedText(text, 255)
        || text.find('|') != std::string_view::npos)
        return false;
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return false;
    auto const first = text.find_first_not_of(' ');
    // The session handler parses commands before its mute checks. Model speech is never a command.
    return first != std::string_view::npos && text[first] != '.' && text[first] != '!' && text[first] != '/';
}

bool IsRemoteSpeech(uint8_t type)
{
    return type == CHAT_MSG_WHISPER || type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER
        || type == CHAT_MSG_CHANNEL;
}

std::string SpeechRouteLabel(SpeechRoute const& route)
{
    switch (route.type)
    {
        case CHAT_MSG_SAY: return "say";
        case CHAT_MSG_YELL: return "yell";
        case CHAT_MSG_WHISPER: return "whisper";
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER: return "party";
        case CHAT_MSG_CHANNEL: return route.channelName;
        default: return "";
    }
}
}
