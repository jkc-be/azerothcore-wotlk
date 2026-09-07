/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "LiveCapture.h"
#include "Creature.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace Alles
{
namespace
{
CaptureResult Omit(CaptureStatus status)
{
    return {status, std::nullopt};
}

Reference CaptureReference(Unit const& unit, LocaleConstant locale)
{
    Reference reference;
    reference.name = unit.GetNameForLocaleIdx(locale);
    if (unit.IsPlayer())
        reference.actor = ActorKey{ActorKind::Player, unit.GetGUID().GetCounter()};
    else if (auto const* creature = unit.ToCreature(); creature && !creature->IsSummon()
        && creature->GetMapId() == 0 && creature->GetInstanceId() == 0 && creature->GetSpawnId() != 0)
        reference.actor = ActorKey{ActorKind::CreatureSpawn, creature->GetSpawnId()};
    return reference;
}

LocaleConstant ObserverLocale(Player const& player)
{
    return player.GetSession() ? player.GetSession()->GetSessionDbcLocale() : DEFAULT_LOCALE;
}

std::string BoundedLabel(char const* value)
{
    return value && IsBoundedText(value, 100) ? value : "";
}

void CaptureSelfContext(Player const& player, Perception& perception)
{
    auto const locale = ObserverLocale(player);
    if (auto const* area = GetAreaEntryByAreaID(player.GetAreaId()))
        perception.place = BoundedLabel(area->area_name[locale]);

    auto const* race = sChrRacesStore.LookupEntry(player.getRace(true));
    auto const* unitClass = sChrClassesStore.LookupEntry(player.getClass());
    perception.selfContext = "I am " + player.GetName() + ", level " + std::to_string(player.GetLevel());
    if (race)
        perception.selfContext += ", " + BoundedLabel(race->name[locale]);
    if (unitClass)
        perception.selfContext += " " + BoundedLabel(unitClass->name[locale]);
    perception.selfContext += ".";

    // The game's own skill slots bound the input; sort for reproducible, bounded self descriptions.
    std::vector<uint32> skills;
    for (auto const& [id, status] : player.GetSkillStatusMap())
        if (status.uState != SKILL_DELETED && player.HasSkill(id))
            skills.push_back(id);
    std::sort(skills.begin(), skills.end());
    std::size_t included = 0;
    for (uint32 const id : skills)
    {
        auto const* skill = sSkillLineStore.LookupEntry(id);
        if (!skill)
            continue;
        auto const label = BoundedLabel(skill->name[locale]);
        if (label.empty())
            continue;
        if (included == 16)
            break;
        auto candidate = perception.selfContext + " I know " + label + " ("
            + std::to_string(player.GetSkillValue(id)) + ").";
        if (!IsBoundedText(candidate, 2048))
            break;
        perception.selfContext = std::move(candidate);
        ++included;
    }
}

bool Comprehends(Player const& receiver, uint32 language)
{
    if (language == LANG_UNIVERSAL)
        return true;
    auto const* descriptor = GetLanguageDescByID(language);
    if (!descriptor || language == LANG_ADDON)
        return false;
    // HandleComprehendLanguage enables the client's comprehension flag without filtering its misc value.
    // The matching aura check below also covers the language-specific ability accepted by the chat handler.
    if (receiver.HasUnitFlag2(UNIT_FLAG2_COMPREHEND_LANG))
        return true;
    if (descriptor->skill_id && receiver.HasSkill(descriptor->skill_id))
        return true;
    for (auto const* aura : receiver.GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE))
        if (aura->GetMiscValue() == int32(language))
            return true;
    return false;
}

CaptureResult Finish(Player const& observer, Perception perception, uint64_t gameTimeMs, uint64_t realTimeMs)
{
    CaptureSelfContext(observer, perception);
    perception.gameTimeMs = gameTimeMs;
    perception.admittedRealTimeMs = realTimeMs;
    if (!GatePerception(perception))
        return Omit(CaptureStatus::InvalidText);
    return {CaptureStatus::Accepted, std::move(perception)};
}

bool VisibleForDeath(Player const& observer, Unit const& subject, float range)
{
    return subject.IsInWorld() && observer.IsInMap(&subject) && observer.InSamePhase(&subject)
        && observer.GetExactDist(&subject) <= range && observer.HaveAtClient(&subject)
        && observer.CanSeeOrDetect(&subject);
}

std::string_view Gesture(uint32 emote)
{
    switch (emote)
    {
        case TEXT_EMOTE_BOW: return "bowed";
        case TEXT_EMOTE_CHEER: return "cheered";
        case TEXT_EMOTE_DANCE: return "danced";
        case TEXT_EMOTE_LAUGH: return "laughed";
        case TEXT_EMOTE_NOD: return "nodded";
        case TEXT_EMOTE_POINT: return "pointed";
        case TEXT_EMOTE_SHAKE: return "shook their head";
        case TEXT_EMOTE_WAVE: return "waved";
        default: return {};
    }
}
}

CaptureResult GateDeliveredPacket(DecodedLocalPacket const& packet, Reference const& source, bool comprehended)
{
    Perception perception;
    perception.source = source;
    perception.language = packet.language;
    switch (packet.kind)
    {
        case LocalPacketKind::Speech:
        case LocalPacketKind::WrittenEmote:
            perception.comprehended = comprehended;
            perception.kind = packet.kind == LocalPacketKind::WrittenEmote && comprehended
                ? PerceptionKind::Emote : PerceptionKind::Speech;
            // Never copy inaccessible text into a perception, including in the written-emote path.
            if (comprehended)
                perception.text = packet.text;
            break;
        case LocalPacketKind::TextEmote:
        {
            auto const gesture = Gesture(packet.textEmote);
            if (gesture.empty())
                return Omit(CaptureStatus::Unsupported);
            perception.kind = PerceptionKind::Emote;
            perception.language = LANG_UNIVERSAL;
            perception.text = (source.name.empty() ? "Someone" : source.name) + " "
                + std::string(gesture) + ".";
            perception.subject.name = packet.targetName;
            break;
        }
        default:
            return Omit(CaptureStatus::Unsupported);
    }
    if (!GatePerception(perception))
        return Omit(CaptureStatus::InvalidText);
    return {CaptureStatus::Accepted, std::move(perception)};
}

CaptureResult CaptureDeliveredPacket(Player& receiver, DecodedLocalPacket const& packet,
    uint64_t gameTimeMs, uint64_t realTimeMs)
{
    if (!receiver.IsInWorld())
        return Omit(CaptureStatus::NotInWorld);
    if (packet.source == receiver.GetGUID())
        return Omit(CaptureStatus::SelfFeedback);

    // Avoid ObjectAccessor's global player lookup: only dereference this safe visibility container.
    auto const* visible = receiver.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
    if (!visible)
        return Omit(CaptureStatus::SourceUnavailable);
    auto const found = visible->find(packet.source);
    if (found == visible->end() || !found->second || !found->second->IsUnit())
        return Omit(CaptureStatus::SourceUnavailable);
    auto const* source = found->second->ToUnit();
    if (!source->IsInWorld() || !receiver.IsInMap(source))
        return Omit(CaptureStatus::SourceUnavailable);

    auto reference = CaptureReference(*source, ObserverLocale(receiver));
    if (packet.chatType == CHAT_MSG_MONSTER_SAY || packet.chatType == CHAT_MSG_MONSTER_YELL
        || packet.chatType == CHAT_MSG_MONSTER_EMOTE)
        reference.name = packet.sourceName;
    auto result = GateDeliveredPacket(packet, reference, Comprehends(receiver, packet.language));
    if (!result.value)
        return result;
    return Finish(receiver, std::move(*result.value), gameTimeMs, realTimeMs);
}

CaptureResult CaptureOwnDeath(Player& player, uint64_t gameTimeMs, uint64_t realTimeMs)
{
    if (!player.IsInWorld())
        return Omit(CaptureStatus::NotInWorld);
    Perception perception;
    perception.kind = PerceptionKind::OwnDeath;
    perception.subject = CaptureReference(player, ObserverLocale(player));
    return Finish(player, std::move(perception), gameTimeMs, realTimeMs);
}

CaptureResult CaptureWitnessedDeath(Player& observer, Unit& victim, Unit* killer, float witnessRange,
    uint64_t gameTimeMs, uint64_t realTimeMs)
{
    if (!observer.IsInWorld() || !victim.IsInWorld())
        return Omit(CaptureStatus::NotInWorld);
    if (&observer == &victim)
        return Omit(CaptureStatus::SelfFeedback);
    if (!std::isfinite(witnessRange) || witnessRange <= 0)
        return Omit(CaptureStatus::InvalidRange);
    if (!VisibleForDeath(observer, victim, witnessRange))
        return Omit(CaptureStatus::NotVisible);

    Perception perception;
    perception.kind = PerceptionKind::WitnessedDeath;
    perception.subject = CaptureReference(victim, ObserverLocale(observer));
    if (killer && killer != &victim && VisibleForDeath(observer, *killer, witnessRange))
        perception.source = CaptureReference(*killer, ObserverLocale(observer));
    return Finish(observer, std::move(perception), gameTimeMs, realTimeMs);
}

CaptureResult CaptureMeeting(Player& observer, Unit const& subject, uint64_t gameTimeMs, uint64_t realTimeMs)
{
    if (!observer.IsInWorld() || !subject.IsInWorld())
        return Omit(CaptureStatus::NotInWorld);
    if (&observer == &subject)
        return Omit(CaptureStatus::SelfFeedback);
    if (!observer.IsInMap(&subject) || !observer.InSamePhase(&subject) || !observer.HaveAtClient(&subject)
        || !observer.CanSeeOrDetect(&subject))
        return Omit(CaptureStatus::NotVisible);

    Perception perception;
    perception.kind = PerceptionKind::Met;
    perception.subject = CaptureReference(subject, ObserverLocale(observer));
    return Finish(observer, std::move(perception), gameTimeMs, realTimeMs);
}
}
