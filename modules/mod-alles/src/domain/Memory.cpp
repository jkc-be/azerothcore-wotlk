/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Memory.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Alles
{
namespace
{
bool IsProbability(double value)
{
    return std::isfinite(value) && value >= 0 && value <= 1;
}

bool IsValidReference(Reference const& reference)
{
    return (!reference.actor || IsValidActor(*reference.actor)) && IsBoundedText(reference.name, 100);
}

std::string NameOrSomeone(Reference const& reference)
{
    return reference.name.empty() ? "someone" : reference.name;
}

void ParseAttribution(Memory& memory, std::string const& text)
{
    // These are exact audible English forms, not a general natural-language parser.
    if (text.starts_with("I saw ") && text.size() > 6)
    {
        memory.claim = text.substr(6);
        memory.attribution = memory.source.name;
        memory.reportedDepth = 1;
    }
    else if (text.starts_with("I heard ") && text.size() > 8)
        memory.claim = text.substr(8);
    else
    {
        auto const separator = text.find(" told me ");
        if (separator != std::string::npos && separator > 0 && separator + 9 < text.size())
        {
            auto const name = text.substr(0, separator);
            // Only a bounded plain name is recognized; arbitrary prose stays an opaque statement.
            if (IsBoundedText(name, 100) && name.find_first_of(".,:;!?\n\r\"") == std::string::npos)
            {
                memory.attribution = name;
                memory.claim = text.substr(separator + 9);
                memory.reportedDepth = 2;
            }
        }
    }
}
}

bool IsBoundedText(std::string_view text, std::size_t maxCharacters)
{
    std::size_t characters = 0;
    for (std::size_t offset = 0; offset < text.size();)
    {
        if (++characters > maxCharacters)
            return false;

        auto const first = static_cast<uint8_t>(text[offset++]);
        uint32_t codepoint = first;
        uint32_t minimum = 0;
        unsigned remaining = 0;
        if (first >= 0xC2 && first <= 0xDF)
        {
            codepoint = first & 0x1F;
            minimum = 0x80;
            remaining = 1;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            codepoint = first & 0x0F;
            minimum = 0x800;
            remaining = 2;
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            codepoint = first & 0x07;
            minimum = 0x10000;
            remaining = 3;
        }
        else if (first == 0 || first >= 0x80)
            return false;

        if (remaining > text.size() - offset)
            return false;

        while (remaining-- > 0)
        {
            auto const next = static_cast<uint8_t>(text[offset++]);
            if ((next & 0xC0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }

        if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
            return false;
    }
    return true;
}

bool IsValidActor(ActorKey owner)
{
    return owner.id != 0 && (owner.kind == ActorKind::Player || owner.kind == ActorKind::CreatureSpawn);
}

bool IsValidPolicy(MemoryPolicy const& policy)
{
    return IsProbability(policy.hearsayCap) && IsProbability(policy.provenanceFloor)
        && IsProbability(policy.forgetBelow) && policy.forgetBelow <= policy.provenanceFloor
        && policy.salienceHalfLifeMs > 0;
}

bool IsValidMemory(Memory const& memory)
{
    return memory.kind <= MemoryKind::Met && memory.formation <= FormationMode::Fallback
        && IsValidReference(memory.subject) && IsValidReference(memory.source)
        && !memory.claim.empty() && IsBoundedText(memory.claim, 512) && IsBoundedText(memory.attribution, 100)
        && IsProbability(memory.confidence) && IsProbability(memory.salience) && memory.contentRevision != 0;
}

bool GatePerception(Perception& perception)
{
    if (perception.kind == PerceptionKind::Speech && !perception.comprehended)
        perception.text.clear();

    return perception.kind <= PerceptionKind::Met && IsValidReference(perception.subject)
        && IsValidReference(perception.source) && IsBoundedText(perception.text, 512)
        && IsBoundedText(perception.place, 100) && IsBoundedText(perception.selfContext, 2048);
}

Memory FormFallback(Perception const& perception, MemoryPolicy const& policy, uint64_t gameTimeMs)
{
    if (!IsValidPolicy(policy))
        throw std::invalid_argument("Invalid alles memory policy");

    auto gated = perception;
    if (!GatePerception(gated))
        throw std::invalid_argument("Invalid alles perception");

    Memory memory;
    memory.subject = gated.subject;
    memory.source = gated.source;
    memory.formedGameTimeMs = gameTimeMs;
    memory.decayGameTimeMs = gameTimeMs;
    memory.confidence = 0.9;
    memory.salience = 0.7;
    switch (gated.kind)
    {
        case PerceptionKind::Speech:
            if (!gated.comprehended)
            {
                memory.kind = MemoryKind::UnintelligibleSpeech;
                memory.claim = "I heard speech I could not understand.";
                break;
            }
            memory.kind = MemoryKind::HeardStatement;
            memory.claim = gated.text.empty() ? "an indistinct statement" : gated.text;
            memory.confidence = std::min(0.5, policy.hearsayCap);
            ParseAttribution(memory, gated.text);
            break;
        case PerceptionKind::Emote:
            memory.kind = MemoryKind::Emote;
            memory.claim = gated.text.empty() ? "I noticed a gesture." : gated.text;
            break;
        case PerceptionKind::WitnessedDeath:
            memory.kind = MemoryKind::WitnessedDeath;
            memory.claim = NameOrSomeone(gated.subject) + " died";
            if (!gated.source.name.empty())
                memory.claim += " after being attacked by " + gated.source.name;
            memory.salience = 1;
            break;
        case PerceptionKind::OwnDeath:
            memory.kind = MemoryKind::OwnDeath;
            memory.claim = "I died.";
            memory.salience = 1;
            break;
        case PerceptionKind::Met:
            memory.kind = MemoryKind::Met;
            memory.claim = "I met " + NameOrSomeone(gated.subject) + ".";
            memory.salience = 0.3;
            memory.formation = FormationMode::Reflex;
            break;
    }
    return memory;
}

std::string RenderMemory(Memory const& memory)
{
    if (memory.kind == MemoryKind::HeardStatement)
    {
        if (!memory.source.name.empty())
        {
            auto const attribution = !memory.attribution.empty() && memory.attribution != memory.source.name
                ? memory.attribution + " reported that " : "";
            return memory.source.name + " told me " + attribution + memory.claim;
        }
        return "I heard " + memory.claim;
    }
    if (memory.kind == MemoryKind::WitnessedDeath)
        return "I saw that " + memory.claim;
    return memory.claim;
}

bool DecayMemory(Memory& memory, MemoryPolicy const& policy, uint64_t gameTimeMs)
{
    if (!IsValidPolicy(policy))
        throw std::invalid_argument("Invalid alles memory policy");
    if (gameTimeMs <= memory.decayGameTimeMs)
        return false;

    auto const elapsed = gameTimeMs - memory.decayGameTimeMs;
    memory.salience *= std::exp2(-static_cast<double>(elapsed) / policy.salienceHalfLifeMs);
    memory.decayGameTimeMs = gameTimeMs;
    if (memory.kind == MemoryKind::HeardStatement && memory.salience < policy.provenanceFloor)
    {
        // Opaque heard text can itself contain attribution. Remove it as well, including from future context.
        std::string const eroded = "something, but I no longer remember what or from whom";
        if (memory.claim != eroded || memory.source != Reference{} || memory.subject != Reference{}
            || !memory.attribution.empty() || memory.reportedDepth)
        {
            memory.claim = eroded;
            memory.source = {};
            memory.subject = {};
            memory.attribution.clear();
            memory.reportedDepth.reset();
            ++memory.contentRevision;
        }
    }
    return true;
}

void RehearseMemory(Memory& memory, MemoryPolicy const& policy, uint64_t gameTimeMs, double strength)
{
    if (!IsProbability(strength))
        throw std::invalid_argument("Invalid alles rehearsal strength");
    DecayMemory(memory, policy, gameTimeMs);
    memory.salience = std::min(1.0, memory.salience + strength);
    // Repetition is not new corroboration. Confidence and erased provenance stay unchanged.
    memory.recalledGameTimeMs = std::max(memory.recalledGameTimeMs, gameTimeMs);
}
}
