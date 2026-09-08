/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_CHAT_AUDIENCE_H
#define MOD_ALLES_CHAT_AUDIENCE_H

#include "SpeechRoute.h"
#include <optional>

class Player;

namespace Alles
{
// World-thread only. Reads identity and actual membership, never the remote speaker's location or quest log.
// Local routes still require the caller's visibility/range gate. This is not evidence that a send succeeded.
std::optional<SpeechRoute> CaptureSpeechRoute(Player& receiver, Player& source, uint8_t type,
    std::string const& channelName = "");
bool OwnsSpeechRoute(Player& speaker, SpeechRoute const& route);
// Chooses only an actually joined audience with another member, or a presently audible local listener.
std::optional<SpeechRoute> QuestionRoute(Player& speaker);
}

#endif
