/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_CHAT_SENDER_H
#define MOD_ALLES_CHAT_SENDER_H

#include "perception/LiveCapture.h"

namespace Alles
{
// All calls require the world thread. A scoped receipt exists only during the ordinary session handler.
bool NormalChatDeliveryPending();
void ObserveNormalChatDelivery(Player const& receiver, CaptureResult const& captured);
// True means this exact comprehended text reached the intended recipient's packet hook. A filtered,
// muted, scripted-away or unobserved send returns false; an attempted action is never a delivery receipt.
bool SendNormalChat(Player& sender, Player& recipient, SpeechRoute const& route, std::string const& text);
// Public question: the receipt requires at least one other actual comprehending recipient.
bool SendNormalQuestion(Player& sender, SpeechRoute const& route, std::string const& text);
}

#endif
