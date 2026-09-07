/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_MEMORY_H
#define MOD_ALLES_MEMORY_H

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Alles
{
enum class ActorKind : uint8_t
{
    Player = 0,
    CreatureSpawn = 1
};

struct ActorKey
{
    ActorKind kind = ActorKind::Player;
    uint64_t id = 0;

    auto operator<=>(ActorKey const&) const = default;
};

// An observed name need not have a persistent identity (for example, a summon).
struct Reference
{
    std::optional<ActorKey> actor;
    std::string name;

    bool operator==(Reference const&) const = default;
};

enum class PerceptionKind : uint8_t
{
    Speech,
    Emote,
    WitnessedDeath,
    OwnDeath,
    Met
};

struct Perception
{
    uint64_t id = 0;
    PerceptionKind kind = PerceptionKind::Speech;
    Reference subject;
    Reference source;
    bool comprehended = true;
    uint32_t language = 0;
    std::string text;
    std::string place;
    std::string selfContext;
    uint64_t gameTimeMs = 0;
    uint64_t admittedRealTimeMs = 0;
    uint64_t emissionId = 0;
};

enum class MemoryKind : uint8_t
{
    HeardStatement,
    UnintelligibleSpeech,
    Emote,
    WitnessedDeath,
    OwnDeath,
    Met
};

enum class FormationMode : uint8_t
{
    Model,
    InProcessFake,
    Reflex,
    Fallback
};

struct Memory
{
    uint64_t id = 0;
    uint64_t contentRevision = 1;
    MemoryKind kind = MemoryKind::HeardStatement;
    Reference subject;
    Reference source;
    std::string claim;
    std::string attribution;
    std::optional<uint32_t> reportedDepth;
    double confidence = 0;
    double salience = 0;
    uint64_t formedGameTimeMs = 0;
    uint64_t recalledGameTimeMs = 0;
    uint64_t decayGameTimeMs = 0;
    FormationMode formation = FormationMode::Fallback;
};

struct MemoryPolicy
{
    double hearsayCap = 0.6;
    double provenanceFloor = 0.2;
    double forgetBelow = 0.01;
    uint64_t salienceHalfLifeMs = 24 * 60 * 60 * uint64_t(1000);
};

// Strict Unicode scalar validation; rejects NUL, overlong encodings and surrogate code points.
bool IsBoundedText(std::string_view text, std::size_t maxCharacters);
bool IsValidActor(ActorKey owner);
bool IsValidPolicy(MemoryPolicy const& policy);
bool IsValidMemory(Memory const& memory);

// Called before a perception enters any retained buffer. Incomprehensible plaintext is destroyed here.
bool GatePerception(Perception& perception);
Memory FormFallback(Perception const& perception, MemoryPolicy const& policy, uint64_t gameTimeMs);
std::string RenderMemory(Memory const& memory);
bool DecayMemory(Memory& memory, MemoryPolicy const& policy, uint64_t gameTimeMs);
void RehearseMemory(Memory& memory, MemoryPolicy const& policy, uint64_t gameTimeMs, double strength);
}

#endif
