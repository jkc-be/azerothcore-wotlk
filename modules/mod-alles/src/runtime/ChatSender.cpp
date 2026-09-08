/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "ChatSender.h"
#include "perception/ChatAudience.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace Alles
{
namespace
{
struct Receipt
{
    ActorKey sender;
    ObjectGuid recipient;
    SpeechRoute route;
    std::string text;
    bool delivered = false;
};

thread_local Receipt* activeReceipt = nullptr;

class ReceiptScope
{
public:
    explicit ReceiptScope(Receipt& receipt) { activeReceipt = &receipt; }
    ~ReceiptScope() { activeReceipt = nullptr; }
};
}

bool NormalChatDeliveryPending()
{
    return activeReceipt != nullptr;
}

void ObserveNormalChatDelivery(Player const& receiver, CaptureResult const& captured)
{
    if (activeReceipt && (!activeReceipt->recipient || receiver.GetGUID() == activeReceipt->recipient)
        && captured.value && captured.route
        && captured.value->comprehended && captured.value->source.actor == activeReceipt->sender
        && *captured.route == activeReceipt->route && captured.value->text == activeReceipt->text)
        activeReceipt->delivered = true;
}

namespace
{
bool Send(Player& sender, Player* recipient, SpeechRoute const& route, std::string const& text)
{
    auto* session = sender.GetSession();
    if (activeReceipt || !session || session->isLogingOut() || !sender.IsInWorld() || !IsSafeChatText(text))
        return false;
    if (recipient)
    {
        auto current = CaptureSpeechRoute(*recipient, sender, route.type, route.channelName);
        if (!current || *current != route)
            return false;
    }
    else if (!OwnsSpeechRoute(sender, route))
        return false;
    Language const language = sender.GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
    WorldPacket request(CMSG_MESSAGECHAT);
    request << uint32(route.type) << uint32(language);
    if (route.type == CHAT_MSG_WHISPER)
        request << recipient->GetName();
    else if (route.type == CHAT_MSG_CHANNEL)
        request << route.channelName;
    request << text;
    Receipt receipt{{ActorKind::Player, sender.GetGUID().GetCounter()},
        recipient ? recipient->GetGUID() : ObjectGuid::Empty, route, text};
    ReceiptScope scope(receipt);
    session->HandleMessagechatOpcode(request);
    return receipt.delivered;
}
}

bool SendNormalChat(Player& sender, Player& recipient, SpeechRoute const& route, std::string const& text)
{
    return Send(sender, &recipient, route, text);
}

bool SendNormalQuestion(Player& sender, SpeechRoute const& route, std::string const& text)
{
    return Send(sender, nullptr, route, text);
}
}
