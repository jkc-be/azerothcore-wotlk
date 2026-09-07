/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "perception/LiveCapture.h"
#include "SharedDefines.h"
#include "gtest/gtest.h"

namespace
{
using namespace Alles;

Reference Speaker()
{
    return {ActorKey{ActorKind::Player, 17}, "Humanc"};
}

TEST(AllesLiveCaptureTest, ForeignSpeechIsRedactedBeforeAnyPerceptionValueExists)
{
    DecodedLocalPacket packet;
    packet.language = LANG_ORCISH;
    packet.text = "Humanb hid the treasure beneath the abbey.";
    auto const captured = GateDeliveredPacket(packet, Speaker(), false);
    ASSERT_EQ(captured.status, CaptureStatus::Accepted);
    ASSERT_TRUE(captured.value.has_value());
    EXPECT_EQ(captured.value->source, Speaker());
    EXPECT_EQ(captured.value->language, LANG_ORCISH);
    EXPECT_FALSE(captured.value->comprehended);
    EXPECT_TRUE(captured.value->text.empty());
    EXPECT_EQ(captured.value->kind, PerceptionKind::Speech);
    auto const memory = FormFallback(*captured.value, {}, 0);
    EXPECT_EQ(memory.kind, MemoryKind::UnintelligibleSpeech);
    EXPECT_EQ(RenderMemory(memory).find("treasure"), std::string::npos);
}

TEST(AllesLiveCaptureTest, ForeignWrittenEmotesCannotBypassTheSpeechLanguageGate)
{
    DecodedLocalPacket packet;
    packet.kind = LocalPacketKind::WrittenEmote;
    packet.language = LANG_ORCISH;
    packet.text = "reveals where the treasure is hidden";
    auto const captured = GateDeliveredPacket(packet, Speaker(), false);
    ASSERT_EQ(captured.status, CaptureStatus::Accepted);
    ASSERT_TRUE(captured.value.has_value());
    EXPECT_EQ(captured.value->kind, PerceptionKind::Speech);
    EXPECT_FALSE(captured.value->comprehended);
    EXPECT_TRUE(captured.value->text.empty());
    EXPECT_EQ(FormFallback(*captured.value, {}, 0).kind, MemoryKind::UnintelligibleSpeech);

    auto const understood = GateDeliveredPacket(packet, Speaker(), true);
    ASSERT_EQ(understood.status, CaptureStatus::Accepted);
    ASSERT_TRUE(understood.value.has_value());
    EXPECT_EQ(understood.value->kind, PerceptionKind::Emote);
    EXPECT_EQ(understood.value->text, packet.text);
}

TEST(AllesLiveCaptureTest, UnderstoodSpeechPreservesExactHeardSourceAndText)
{
    DecodedLocalPacket packet;
    packet.language = LANG_COMMON;
    packet.text = "Humand told me Humanb died";
    auto const captured = GateDeliveredPacket(packet, Speaker(), true);
    ASSERT_EQ(captured.status, CaptureStatus::Accepted);
    ASSERT_TRUE(captured.value.has_value());
    EXPECT_EQ(captured.value->text, packet.text);
    EXPECT_EQ(captured.value->source, Speaker());
    auto const memory = FormFallback(*captured.value, {}, 0);
    EXPECT_EQ(memory.source, Speaker());
    EXPECT_EQ(memory.attribution, "Humand");
}

TEST(AllesLiveCaptureTest, TextEmotesDescribeOnlyKnownGesturesWithoutInventingTargetIdentity)
{
    DecodedLocalPacket packet;
    packet.kind = LocalPacketKind::TextEmote;
    packet.textEmote = TEXT_EMOTE_WAVE;
    packet.targetName = "Humanb";
    auto const captured = GateDeliveredPacket(packet, Speaker(), false);
    ASSERT_EQ(captured.status, CaptureStatus::Accepted);
    ASSERT_TRUE(captured.value.has_value());
    EXPECT_EQ(captured.value->kind, PerceptionKind::Emote);
    EXPECT_TRUE(captured.value->comprehended);
    EXPECT_EQ(captured.value->language, LANG_UNIVERSAL);
    EXPECT_EQ(captured.value->text, "Humanc waved.");
    EXPECT_EQ(captured.value->subject.name, "Humanb");
    EXPECT_FALSE(captured.value->subject.actor.has_value());

    packet.textEmote = 0;
    auto const unsupported = GateDeliveredPacket(packet, Speaker(), false);
    EXPECT_EQ(unsupported.status, CaptureStatus::Unsupported);
    EXPECT_FALSE(unsupported.value.has_value());
}

TEST(AllesLiveCaptureTest, InvalidRetainedNamesAndOversizedUnderstoodTextOmitTheObservation)
{
    DecodedLocalPacket packet;
    packet.text = std::string(513, 'x');
    auto const oversized = GateDeliveredPacket(packet, Speaker(), true);
    EXPECT_EQ(oversized.status, CaptureStatus::InvalidText);
    EXPECT_FALSE(oversized.value.has_value());

    packet.text = "Humanb died";
    auto invalidSource = Speaker();
    invalidSource.name = "\xED\xA0\x80";
    auto const invalid = GateDeliveredPacket(packet, invalidSource, true);
    EXPECT_EQ(invalid.status, CaptureStatus::InvalidText);
    EXPECT_FALSE(invalid.value.has_value());
}
}
