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
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

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

bool PlayerKilledByPlayer(Reference const& victim, Reference const& killer)
{
    return victim.actor && killer.actor && victim.actor->kind == ActorKind::Player
        && killer.actor->kind == ActorKind::Player && victim.actor != killer.actor;
}

bool PlainName(std::string_view name)
{
    return !name.empty() && IsBoundedText(name, 100)
        && name.find_first_of(".,:;!?\n\r\"") == std::string_view::npos;
}

void ParseAttribution(Memory& memory, std::string text)
{
    // Only exact audible forms are recognized. The deepest named source remains an attribution, not evidence.
    uint32_t depth = memory.reportedDepth.value_or(1);
    while (!text.empty())
    {
        if (text.starts_with("I saw ") && text.size() > 6)
        {
            text.erase(0, text.starts_with("I saw that ") ? 11 : 6);
            if (memory.attribution.empty())
                memory.attribution = memory.source.name;
            memory.reportedDepth = depth;
            break;
        }
        if (text.starts_with("I heard ") && text.size() > 8)
        {
            text.erase(0, 8);
            memory.reportedDepth.reset();
            break; // The number of unnamed intermediaries is unknown.
        }
        bool parsed = false;
        auto const firstMarker = std::min(text.find(" told me "), text.find(" reported that "));
        for (std::string_view const marker : {" told me ", " reported that "})
        {
            auto const separator = text.find(marker);
            if (separator == std::string::npos || separator != firstMarker
                || !PlainName(std::string_view(text).substr(0, separator))
                || separator + marker.size() >= text.size())
                continue;
            memory.attribution = text.substr(0, separator);
            text.erase(0, separator + marker.size());
            if (depth < std::numeric_limits<uint32_t>::max())
                ++depth;
            memory.reportedDepth = depth;
            parsed = true;
            break;
        }
        if (!parsed)
            break;
    }
    if (!text.empty())
        memory.claim = std::move(text);
}

bool RoutineGreeting(std::string_view text)
{
    std::string normalized;
    for (unsigned char c : text)
        normalized += c < 128 && !std::isalnum(c) ? ' ' : char(c < 128 ? std::tolower(c) : c);
    std::istringstream input(normalized);
    std::vector<std::string> words;
    for (std::string word; input >> word;)
        words.push_back(std::move(word));
    if (words.empty() || (words[0] != "hello" && words[0] != "hi" && words[0] != "hey"
        && words[0] != "greetings" && words[0] != "it" && words[0] != "good" && words[0] != "nice"))
        return false;
    // Whole utterance patterns only: a greeting followed by useful information must remain available.
    // Compile these once; salience ceilings are evaluated repeatedly during world updates.
    static auto const patterns = []
    {
        std::vector<std::vector<std::string>> result;
        for (auto const* phrase : {"it is good to see you", "it s good to see you", "good to see you",
            "nice to meet you", "it is nice to meet you"})
            for (auto const* suffix : {"", " *", " too", " too *", " again", " again *"})
            {
                std::istringstream parts(std::string(phrase) + suffix);
                std::vector<std::string> pattern;
                for (std::string part; parts >> part;)
                    pattern.push_back(std::move(part));
                result.push_back(std::move(pattern));
            }
        return result;
    }();
    for (std::size_t start : {0u, 1u, 2u})
    {
        if (start && (words[0] != "hello" && words[0] != "hi" && words[0] != "hey" && words[0] != "greetings"))
            continue;
        if (start && start == words.size())
            return true;
        for (auto const& pattern : patterns)
        {
            if (start + pattern.size() != words.size())
                continue;
            bool matches = true;
            for (std::size_t i = 0; i < pattern.size(); ++i)
                matches = matches && (pattern[i] == "*" || pattern[i] == words[start + i]);
            if (matches)
                return true;
        }
    }
    return false;
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
            memory.claim = gated.source.name.empty() ? "I died."
                : "I died after being attacked by " + gated.source.name + ".";
            memory.salience = 1;
            break;
        case PerceptionKind::Met:
            memory.kind = MemoryKind::Met;
            memory.claim = "I met " + NameOrSomeone(gated.subject) + ".";
            memory.salience = 0.3;
            memory.formation = FormationMode::Reflex;
            break;
    }
    memory.salience = std::min(memory.salience, SalienceCeiling(memory));
    if (UsesReflexFormation(gated))
        memory.formation = FormationMode::Reflex;
    return memory;
}

bool UsesReflexFormation(Perception const& perception)
{
    if (perception.kind == PerceptionKind::Speech && perception.comprehended)
    {
        Memory heard;
        heard.claim = perception.text;
        ParseAttribution(heard, heard.claim);
        if (IsRoutineMemory(heard))
            return true;
    }
    return perception.kind == PerceptionKind::Met
        || ((perception.kind == PerceptionKind::WitnessedDeath || perception.kind == PerceptionKind::OwnDeath)
            && !PlayerKilledByPlayer(perception.subject, perception.source));
}

bool IsRoutineMemory(Memory const& memory)
{
    return memory.kind == MemoryKind::HeardStatement && RoutineGreeting(memory.claim);
}

bool CanShareMemory(Memory const& memory, bool relevantQuestion)
{
    if (IsRoutineMemory(memory))
        return false;
    // Unprompted reports must come from this character's own observation. Conversation has its own reply policy.
    return memory.kind == MemoryKind::WitnessedDeath
        || (memory.kind == MemoryKind::HeardStatement && relevantQuestion);
}

double SalienceCeiling(Memory const& memory)
{
    if (IsRoutineMemory(memory))
        return 0.05;
    if ((memory.kind == MemoryKind::WitnessedDeath || memory.kind == MemoryKind::OwnDeath)
        && !PlayerKilledByPlayer(memory.subject, memory.source))
        return 0.05;
    return 1;
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
    bool normalized = false;
    if (memory.kind == MemoryKind::HeardStatement)
    {
        auto const claim = memory.claim;
        auto const attribution = memory.attribution;
        auto const depth = memory.reportedDepth;
        ParseAttribution(memory, memory.claim);
        normalized = claim != memory.claim || attribution != memory.attribution || depth != memory.reportedDepth;
        if (normalized)
            ++memory.contentRevision;
    }
    double const previous = memory.salience;
    memory.salience = std::min(memory.salience, SalienceCeiling(memory));
    if (gameTimeMs <= memory.decayGameTimeMs)
        return normalized || memory.salience != previous;

    auto const elapsed = gameTimeMs - memory.decayGameTimeMs;
    bool const routine = IsRoutineMemory(memory);
    auto const halfLife = routine ? std::min(policy.salienceHalfLifeMs, uint64_t(600000))
        : policy.salienceHalfLifeMs;
    memory.salience *= std::exp2(-static_cast<double>(elapsed) / halfLife);
    memory.decayGameTimeMs = gameTimeMs;
    if (memory.kind == MemoryKind::HeardStatement && !routine && memory.salience < policy.provenanceFloor)
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
    memory.salience = std::min(SalienceCeiling(memory), memory.salience + strength);
    // Repetition is not new corroboration. Confidence and erased provenance stay unchanged.
    memory.recalledGameTimeMs = std::max(memory.recalledGameTimeMs, gameTimeMs);
}
}
