/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PacketDecoder.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "domain/Memory.h"
#include <string_view>
#include <utility>

namespace Alles
{
namespace
{
constexpr std::size_t MaxPacketBytes = 4096;

class PacketReader
{
public:
    explicit PacketReader(WorldPacket const& packet) : _packet(packet) { }

    template<typename T>
    bool Number(T& result)
    {
        if (sizeof(T) > _packet.size() - _position)
            return Fail(PacketDecodeStatus::Malformed);

        // WoW wire integers are little-endian, independently of the host's byte order.
        uint64 value = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index)
            value |= uint64(_packet.contents()[_position++]) << (8 * index);
        result = static_cast<T>(value);
        return true;
    }

    bool Guid(ObjectGuid& result)
    {
        uint64 raw = 0;
        if (!Number(raw))
            return false;
        result = ObjectGuid(raw);
        return true;
    }

    bool SizedText(std::string& result, std::size_t maxCharacters)
    {
        uint32 bytes = 0;
        return Number(bytes) && Text(result, bytes, maxCharacters);
    }

    bool CString(std::string& result, std::size_t maxCharacters)
    {
        std::size_t bytes = 0;
        while (bytes < _packet.size() - _position && _packet.contents()[_position + bytes] != 0)
        {
            if (++bytes > maxCharacters * 4)
                return Fail(PacketDecodeStatus::TooLarge);
        }
        return Text(result, uint32(bytes + 1), maxCharacters);
    }

    bool Text(std::string& result, uint32 bytesWithTerminator, std::size_t maxCharacters)
    {
        if (bytesWithTerminator == 0)
            return Fail(PacketDecodeStatus::Malformed);
        if (bytesWithTerminator > maxCharacters * 4 + 1)
            return Fail(PacketDecodeStatus::TooLarge);
        if (bytesWithTerminator > _packet.size() - _position)
            return Fail(PacketDecodeStatus::Malformed);

        auto const* start = reinterpret_cast<char const*>(_packet.contents() + _position);
        if (start[bytesWithTerminator - 1] != '\0')
            return Fail(PacketDecodeStatus::Malformed);
        std::string_view const text(start, bytesWithTerminator - 1);
        if (!IsBoundedText(text, maxCharacters))
            return Fail(PacketDecodeStatus::InvalidText);
        result.assign(text);
        _position += bytesWithTerminator;
        return true;
    }

    bool Finished() const { return _position == _packet.size(); }
    PacketDecodeStatus Status() const { return _status; }

private:
    bool Fail(PacketDecodeStatus status)
    {
        _status = status;
        return false;
    }

    WorldPacket const& _packet;
    std::size_t _position = 0;
    PacketDecodeStatus _status = PacketDecodeStatus::Malformed;
};

PacketDecodeResult Failure(PacketDecodeStatus status)
{
    return {status, std::nullopt};
}
}

PacketDecodeResult DecodeLocalPacket(WorldPacket const& packet)
{
    if (packet.GetOpcode() != SMSG_MESSAGECHAT && packet.GetOpcode() != SMSG_GM_MESSAGECHAT
        && packet.GetOpcode() != SMSG_TEXT_EMOTE)
        return Failure(PacketDecodeStatus::Unsupported);
    if (packet.size() > MaxPacketBytes)
        return Failure(PacketDecodeStatus::TooLarge);

    PacketReader reader(packet);
    DecodedLocalPacket decoded;
    if (packet.GetOpcode() == SMSG_TEXT_EMOTE)
    {
        decoded.kind = LocalPacketKind::TextEmote;
        uint32 targetBytes = 0;
        if (!reader.Guid(decoded.source) || !reader.Number(decoded.textEmote)
            || !reader.Number(decoded.emoteVariation) || !reader.Number(targetBytes))
            return Failure(reader.Status());
        if (targetBytes > 400)
            return Failure(PacketDecodeStatus::TooLarge);

        // EmoteChatBuilder's length excludes NUL; lengths zero and one both serialize an empty name.
        if (!reader.Text(decoded.targetName, targetBytes > 1 ? targetBytes + 1 : 1, 100))
            return Failure(reader.Status());
    }
    else
    {
        if (!reader.Number(decoded.chatType))
            return Failure(reader.Status());
        bool monster = false;
        switch (decoded.chatType)
        {
            case CHAT_MSG_SAY:
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_CHANNEL:
                break;
            case CHAT_MSG_EMOTE:
            case CHAT_MSG_TEXT_EMOTE:
                decoded.kind = LocalPacketKind::WrittenEmote;
                break;
            case CHAT_MSG_MONSTER_SAY:
            case CHAT_MSG_MONSTER_YELL:
                monster = true;
                break;
            case CHAT_MSG_MONSTER_EMOTE:
                monster = true;
                decoded.kind = LocalPacketKind::WrittenEmote;
                break;
            default:
                return Failure(PacketDecodeStatus::Unsupported);
        }

        uint32 flags = 0;
        if (!reader.Number(decoded.language) || !reader.Guid(decoded.source) || !reader.Number(flags))
            return Failure(reader.Status());
        if (decoded.language == LANG_ADDON)
            return Failure(PacketDecodeStatus::Unsupported);
        if ((monster || packet.GetOpcode() == SMSG_GM_MESSAGECHAT) && !reader.SizedText(decoded.sourceName, 100))
            return Failure(reader.Status());
        if (decoded.chatType == CHAT_MSG_CHANNEL)
        {
            if (!reader.CString(decoded.channelName, 100))
                return Failure(reader.Status());
            if (decoded.channelName.empty())
                return Failure(PacketDecodeStatus::Malformed);
        }
        if (!reader.Guid(decoded.target))
            return Failure(reader.Status());
        if (monster && decoded.target && !decoded.target.IsPlayer() && !decoded.target.IsPet())
            if (!reader.SizedText(decoded.targetName, 100))
                return Failure(reader.Status());

        uint8 chatTag = 0;
        if (!reader.SizedText(decoded.text, 512) || !reader.Number(chatTag))
            return Failure(reader.Status());
    }

    if (decoded.source.IsEmpty() || !reader.Finished())
        return Failure(PacketDecodeStatus::Malformed);
    return {PacketDecodeStatus::Accepted, std::move(decoded)};
}
}
