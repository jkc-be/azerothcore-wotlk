/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "perception/PacketDecoder.h"
#include "Chat.h"
#include "WorldPacket.h"
#include "gtest/gtest.h"
#include <limits>

namespace
{
using namespace Alles;

ObjectGuid PlayerSource()
{
    return ObjectGuid(HighGuid::Player, uint32(17));
}

ObjectGuid CreatureSource()
{
    return ObjectGuid(HighGuid::Unit, uint32(299), uint32(17));
}

bool IsMonster(ChatMsg type)
{
    return type == CHAT_MSG_MONSTER_SAY || type == CHAT_MSG_MONSTER_YELL || type == CHAT_MSG_MONSTER_EMOTE;
}

WorldPacket ChatPacket(ChatMsg type, std::string const& text = "Humanb died", ObjectGuid target = {})
{
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, type, LANG_COMMON, IsMonster(type) ? CreatureSource() : PlayerSource(),
        target, text, 0, "Young Wolf", "Humanb");
    return packet;
}

std::string RepeatScalar(std::string const& scalar, std::size_t count)
{
    std::string text;
    for (std::size_t index = 0; index < count; ++index)
        text += scalar;
    return text;
}

void ExpectRejected(WorldPacket const& packet)
{
    auto const result = DecodeLocalPacket(packet);
    EXPECT_NE(result.status, PacketDecodeStatus::Accepted);
    EXPECT_FALSE(result.value.has_value());
}

class AllesLocalChatPacketTest : public ::testing::TestWithParam<ChatMsg> { };

TEST_P(AllesLocalChatPacketTest, CoreBuilderRoundTripPreservesOnlyDeliveredValues)
{
    auto const type = GetParam();
    auto packet = ChatPacket(type);
    packet.rpos(2);
    auto const result = DecodeLocalPacket(packet);
    ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
    ASSERT_TRUE(result.value.has_value());
    auto const& decoded = *result.value;
    EXPECT_EQ(decoded.chatType, type);
    EXPECT_EQ(decoded.source, IsMonster(type) ? CreatureSource() : PlayerSource());
    EXPECT_TRUE(decoded.target.IsEmpty());
    EXPECT_EQ(decoded.sourceName, IsMonster(type) ? "Young Wolf" : "");
    EXPECT_TRUE(decoded.targetName.empty());
    EXPECT_EQ(decoded.text, "Humanb died");
    EXPECT_EQ(decoded.language, LANG_COMMON);
    EXPECT_EQ(packet.rpos(), 2u);
    EXPECT_EQ(decoded.kind,
        type == CHAT_MSG_SAY || type == CHAT_MSG_YELL || type == CHAT_MSG_MONSTER_SAY || type == CHAT_MSG_MONSTER_YELL
            ? LocalPacketKind::Speech : LocalPacketKind::WrittenEmote);
}

TEST_P(AllesLocalChatPacketTest, EveryTruncatedPrefixAndTrailingDataAreRejected)
{
    auto const packet = ChatPacket(GetParam());
    for (std::size_t length = 0; length < packet.size(); ++length)
    {
        SCOPED_TRACE(length);
        auto truncated = packet;
        truncated.resize(length);
        ExpectRejected(truncated);
    }
    auto trailing = packet;
    trailing << uint8(0);
    ExpectRejected(trailing);
}

TEST_P(AllesLocalChatPacketTest, UnicodeMessageIsBoundedByScalars)
{
    auto const valid = ChatPacket(GetParam(), RepeatScalar("\xF0\x9F\x90\xBA", 512));
    auto const result = DecodeLocalPacket(valid);
    ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->text, RepeatScalar("\xF0\x9F\x90\xBA", 512));
    ExpectRejected(ChatPacket(GetParam(), RepeatScalar("\xF0\x9F\x90\xBA", 513)));
    ExpectRejected(ChatPacket(GetParam(), std::string(513, 'x')));
    ExpectRejected(ChatPacket(GetParam(), "\xED\xA0\x80"));
    ExpectRejected(ChatPacket(GetParam(), "\xC0\xAF"));
    ExpectRejected(ChatPacket(GetParam(), "\xE7\x8B"));
    ExpectRejected(ChatPacket(GetParam(), std::string("a\0b", 3)));
}

TEST_P(AllesLocalChatPacketTest, MessageLengthAndTerminatorMustMatchExactly)
{
    auto packet = ChatPacket(GetParam());
    auto const lengthPosition = packet.size() - std::string("Humanb died").size() - 6;
    auto badLength = packet;
    badLength.put<uint32>(lengthPosition, 0);
    ExpectRejected(badLength);
    badLength = packet;
    badLength.put<uint32>(lengthPosition, std::numeric_limits<uint32>::max());
    EXPECT_EQ(DecodeLocalPacket(badLength).status, PacketDecodeStatus::TooLarge);
    badLength = packet;
    badLength.put<uint32>(lengthPosition, 4);
    ExpectRejected(badLength);
    packet.put<uint8>(packet.size() - 2, 'x');
    ExpectRejected(packet);
}

INSTANTIATE_TEST_SUITE_P(CoreBuilders, AllesLocalChatPacketTest,
    ::testing::Values(CHAT_MSG_SAY, CHAT_MSG_YELL, CHAT_MSG_EMOTE, CHAT_MSG_TEXT_EMOTE,
        CHAT_MSG_MONSTER_SAY, CHAT_MSG_MONSTER_YELL, CHAT_MSG_MONSTER_EMOTE));

TEST(AllesPacketDecoderTest, MonsterTargetNameLayoutDependsOnTargetGuidType)
{
    ObjectGuid const targets[] =
    {
        {},
        ObjectGuid(HighGuid::Player, uint32(18)),
        ObjectGuid(HighGuid::Pet, uint32(299), uint32(18)),
        ObjectGuid(HighGuid::Unit, uint32(299), uint32(18))
    };
    for (auto const target : targets)
    {
        auto const result = DecodeLocalPacket(ChatPacket(CHAT_MSG_MONSTER_SAY, "Humanb died", target));
        ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
        ASSERT_TRUE(result.value.has_value());
        EXPECT_EQ(result.value->target, target);
        EXPECT_EQ(result.value->targetName, target && !target.IsPlayer() && !target.IsPet() ? "Humanb" : "");
        EXPECT_EQ(result.value->text, "Humanb died");
    }
}

TEST(AllesPacketDecoderTest, MonsterNamesUseNpcUnicodeLimits)
{
    WorldPacket packet;
    auto const name = RepeatScalar("\xF0\x9F\x90\xBA", 100);
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_MONSTER_EMOTE, LANG_UNIVERSAL, CreatureSource(), CreatureSource(),
        "bows", 0, name, name);
    auto const result = DecodeLocalPacket(packet);
    ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->sourceName, name);
    EXPECT_EQ(result.value->targetName, name);

    for (auto const& invalid : {name + "x", std::string("\x80"), std::string("a\0b", 3)})
    {
        ChatHandler::BuildChatPacket(packet, CHAT_MSG_MONSTER_SAY, LANG_UNIVERSAL, CreatureSource(), {},
            "bows", 0, invalid);
        ExpectRejected(packet);
        ChatHandler::BuildChatPacket(packet, CHAT_MSG_MONSTER_SAY, LANG_UNIVERSAL, CreatureSource(), CreatureSource(),
            "bows", 0, name, invalid);
        ExpectRejected(packet);
    }
}

TEST(AllesPacketDecoderTest, LanguageRemainsDeliveryMetadataUntilSafeComprehensionCapture)
{
    for (uint32 const language : {uint32(LANG_UNIVERSAL), uint32(LANG_ORCISH), uint32(123456)})
    {
        auto packet = ChatPacket(CHAT_MSG_SAY, "a secret the recipient may not understand");
        packet.put<uint32>(1, language);
        auto const result = DecodeLocalPacket(packet);
        ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
        ASSERT_TRUE(result.value.has_value());
        EXPECT_EQ(result.value->language, language);
        EXPECT_EQ(result.value->text, "a secret the recipient may not understand");
    }
}

TEST(AllesPacketDecoderTest, UnsupportedLocalityAndOpcodesNeverProduceValues)
{
    ChatMsg const unsupported[] =
    {
        CHAT_MSG_WHISPER, CHAT_MSG_WHISPER_FOREIGN, CHAT_MSG_MONSTER_WHISPER, CHAT_MSG_RAID_BOSS_WHISPER,
        CHAT_MSG_PARTY, CHAT_MSG_MONSTER_PARTY, CHAT_MSG_GUILD, CHAT_MSG_SYSTEM, CHAT_MSG_RAID_BOSS_EMOTE,
        static_cast<ChatMsg>(255)
    };
    for (auto const type : unsupported)
    {
        auto const result = DecodeLocalPacket(ChatPacket(type));
        EXPECT_EQ(result.status, PacketDecodeStatus::Unsupported);
        EXPECT_FALSE(result.value.has_value());
    }

    for (auto const opcode : {SMSG_EMOTE, CMSG_MESSAGECHAT, SMSG_UPDATE_OBJECT})
    {
        auto packet = ChatPacket(CHAT_MSG_SAY);
        packet.SetOpcode(opcode);
        EXPECT_EQ(DecodeLocalPacket(packet).status, PacketDecodeStatus::Unsupported);
    }

    auto addon = ChatPacket(CHAT_MSG_SAY);
    addon.put<uint32>(1, LANG_ADDON);
    EXPECT_EQ(DecodeLocalPacket(addon).status, PacketDecodeStatus::Unsupported);
}

TEST(AllesPacketDecoderTest, PrivilegedLocalChatUsesTheCoreGmPacketLayout)
{
    for (auto const type : {CHAT_MSG_SAY, CHAT_MSG_YELL, CHAT_MSG_EMOTE, CHAT_MSG_TEXT_EMOTE})
    {
        WorldPacket packet;
        ChatHandler::BuildChatPacket(packet, type, LANG_COMMON, PlayerSource(), {}, "HELP", 0,
            "Josh", "", 0, true);
        ASSERT_EQ(packet.GetOpcode(), SMSG_GM_MESSAGECHAT);
        auto const result = DecodeLocalPacket(packet);
        ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
        EXPECT_EQ(result.value->source, PlayerSource());
        EXPECT_EQ(result.value->sourceName, "Josh");
        EXPECT_EQ(result.value->text, "HELP");
        for (std::size_t length = 0; length < packet.size(); ++length)
        {
            auto truncated = packet;
            truncated.resize(length);
            ExpectRejected(truncated);
        }
        packet << uint8(0);
        ExpectRejected(packet);
    }
    WorldPacket whisper;
    ChatHandler::BuildChatPacket(whisper, CHAT_MSG_WHISPER, LANG_COMMON, PlayerSource(), {}, "private", 0,
        "Josh", "", 0, true);
    EXPECT_EQ(DecodeLocalPacket(whisper).status, PacketDecodeStatus::Unsupported);
}

TEST(AllesPacketDecoderTest, EmptySourceAndExcessivePacketSizeAreRejected)
{
    auto packet = ChatPacket(CHAT_MSG_SAY);
    packet.put<uint64>(5, 0);
    ExpectRejected(packet);
    packet.resize(4097);
    EXPECT_EQ(DecodeLocalPacket(packet).status, PacketDecodeStatus::TooLarge);
}

// Acore::EmoteChatBuilder is private to ChatHandler.cpp and requires live Player/Unit objects. These fixtures
// encode its distinct wire layout directly; chat-type TEXT_EMOTE above uses the public core packet builder.
WorldPacket TextEmotePacket(std::string const& target, uint32 declaredBytes)
{
    WorldPacket packet(SMSG_TEXT_EMOTE);
    packet << PlayerSource() << uint32(2) << uint32(7) << declaredBytes;
    if (declaredBytes > 1)
        packet << target;
    else
        packet << uint8(0);
    return packet;
}

TEST(AllesPacketDecoderTest, TextEmoteHasAnIdentifierAndOptionalTargetInsteadOfSpeech)
{
    auto const result = DecodeLocalPacket(TextEmotePacket("Humanb", 6));
    ASSERT_EQ(result.status, PacketDecodeStatus::Accepted);
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->kind, LocalPacketKind::TextEmote);
    EXPECT_EQ(result.value->source, PlayerSource());
    EXPECT_EQ(result.value->textEmote, 2u);
    EXPECT_EQ(result.value->emoteVariation, 7u);
    EXPECT_EQ(result.value->targetName, "Humanb");
    EXPECT_TRUE(result.value->target.IsEmpty());
    EXPECT_TRUE(result.value->text.empty());

    for (uint32 const shortLength : {0u, 1u})
    {
        auto const empty = DecodeLocalPacket(TextEmotePacket("", shortLength));
        ASSERT_EQ(empty.status, PacketDecodeStatus::Accepted);
        ASSERT_TRUE(empty.value.has_value());
        EXPECT_TRUE(empty.value->targetName.empty());
    }
}

TEST(AllesPacketDecoderTest, TextEmoteLengthsUnicodeTruncationAndTrailingBytesAreBounded)
{
    auto const name = RepeatScalar("\xF0\x9F\x90\xBA", 100);
    EXPECT_EQ(DecodeLocalPacket(TextEmotePacket(name, 400)).status, PacketDecodeStatus::Accepted);
    ExpectRejected(TextEmotePacket(name + "x", 401));
    ExpectRejected(TextEmotePacket("\xED\xA0\x80", 3));
    ExpectRejected(TextEmotePacket(std::string("a\0b", 3), 3));
    ExpectRejected(TextEmotePacket("Humanb", 5));
    ExpectRejected(TextEmotePacket("Humanb", 7));
    ExpectRejected(TextEmotePacket("Humanb", std::numeric_limits<uint32>::max()));

    auto const packet = TextEmotePacket("Humanb", 6);
    for (std::size_t length = 0; length < packet.size(); ++length)
    {
        SCOPED_TRACE(length);
        auto truncated = packet;
        truncated.resize(length);
        ExpectRejected(truncated);
    }
    auto trailing = packet;
    trailing << uint8(0);
    ExpectRejected(trailing);
}
}
