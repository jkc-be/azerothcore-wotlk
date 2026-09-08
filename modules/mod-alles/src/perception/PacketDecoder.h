/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_PACKET_DECODER_H
#define MOD_ALLES_PACKET_DECODER_H

#include "ObjectGuid.h"
#include <optional>
#include <string>

class WorldPacket;

namespace Alles
{
enum class PacketDecodeStatus
{
    Accepted,
    Unsupported,
    Malformed,
    TooLarge,
    InvalidText
};

enum class LocalPacketKind
{
    Speech,
    WrittenEmote,
    TextEmote
};

// Ephemeral delivery bytes, not a retained perception: capture comprehension in the safe delivery context
// and destroy unintelligible text before enqueueing anything. Runtime GUIDs are not persistent ActorKeys.
struct DecodedLocalPacket
{
    LocalPacketKind kind = LocalPacketKind::Speech;
    uint8 chatType = 0;
    uint32 language = 0;
    ObjectGuid source;
    ObjectGuid target;
    std::string sourceName;
    std::string targetName;
    std::string channelName;
    std::string text;
    uint32 textEmote = 0;
    uint32 emoteVariation = 0;
};

struct PacketDecodeResult
{
    PacketDecodeStatus status = PacketDecodeStatus::Unsupported;
    std::optional<DecodedLocalPacket> value;
};

// Accepted means a supported wire layout, not verified local delivery: creature_text can use local-looking
// types for zone/map announcements. The capture adapter must also check delivery scope/emission metadata.
// Does not move the packet read cursor, resolve objects, interpret language or emit animation observations.
PacketDecodeResult DecodeLocalPacket(WorldPacket const& packet);
}

#endif
