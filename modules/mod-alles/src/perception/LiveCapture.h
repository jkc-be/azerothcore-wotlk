/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_LIVE_CAPTURE_H
#define MOD_ALLES_LIVE_CAPTURE_H

#include "PacketDecoder.h"
#include "SpeechRoute.h"
#include "domain/Memory.h"

class Player;
class Unit;

namespace Alles
{
enum class CaptureStatus
{
    Accepted,
    NotInWorld,
    SelfFeedback,
    SourceUnavailable,
    NotVisible,
    InvalidRange,
    InvalidText,
    Unsupported
};

// Callers count omitted outcomes and enqueue only value. These helpers own no mutable global state.
struct CaptureResult
{
    CaptureStatus status = CaptureStatus::Unsupported;
    std::optional<Perception> value;
    std::optional<SpeechRoute> route;
};

// Pure policy seam after source/comprehension have been captured. Text is gated even for written emotes.
CaptureResult GateDeliveredPacket(DecodedLocalPacket const& packet, Reference const& source, bool comprehended);

// THREAD SAFETY: requires the world thread after map workers join. A SendPacket callback alone is NOT
// proof of this precondition. Local sources resolve from receiver visibility; delivered remote speech
// resolves only player identity globally, with the receiver's channel/group membership as routing evidence.
CaptureResult CaptureDeliveredPacket(Player& receiver, DecodedLocalPacket const& packet,
    uint64_t gameTimeMs, uint64_t realTimeMs);

// THREAD SAFETY: own death requires the player's safe gameplay context. Witness capture requires a safe
// victim-map callback with observer/victim/killer still valid; caller must prefilter observer to that map.
// No returned value contains a pointer. Self death is excluded from witness capture to avoid duplicate paths.
CaptureResult CaptureOwnDeath(Player& player, uint64_t gameTimeMs, uint64_t realTimeMs, Unit* killer = nullptr);
CaptureResult CaptureWitnessedDeath(Player& observer, Unit& victim, Unit* killer, float witnessRange,
    uint64_t gameTimeMs, uint64_t realTimeMs);

// THREAD SAFETY: call during a safe same-map scan or after map workers join. The caller owns visibility-set
// diffing/cooldowns and must count/omit unsafe contexts before reading either object or invoking this helper.
CaptureResult CaptureMeeting(Player& observer, Unit const& subject, uint64_t gameTimeMs, uint64_t realTimeMs);
}

#endif
