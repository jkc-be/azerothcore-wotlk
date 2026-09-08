/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "runtime/ChatSender.h"
#include "SharedDefines.h"
#include "gtest/gtest.h"

TEST(AllesChatSenderTest, ModelSpeechCannotEnterTheCommandParserOrChatMarkup)
{
    for (auto const* text : {".server shutdown 1", "!die", "  .modify money 100", "/invite Humanb",
        "|Hitem:1|h", "hello\n.world", "hello\tthere", "", "   ", "\x7f", "\x80"})
        EXPECT_FALSE(Alles::IsSafeChatText(text)) << text;
    EXPECT_FALSE(Alles::IsSafeChatText(std::string("hello\0there", 11)));
    EXPECT_FALSE(Alles::IsSafeChatText(std::string(256, 'x')));
    EXPECT_TRUE(Alles::IsSafeChatText("Where can I find work?"));
    EXPECT_TRUE(Alles::IsSafeChatText("I heard there's work nearby. Shall we ask?"));
    EXPECT_TRUE(Alles::IsSafeChatText("Bonjour, o\xC3\xB9 allez-vous?"));
}

TEST(AllesChatSenderTest, RouteIdentityKeepsAreasAndPartySubgroupsSeparate)
{
    Alles::SpeechRoute general{CHAT_MSG_CHANNEL, 1, "General - Elwynn Forest"};
    auto changed = general;
    changed.channelName = "General - Westfall";
    EXPECT_NE(general, changed);
    changed = general;
    changed.channelId = 0;
    EXPECT_NE(general, changed);
    Alles::SpeechRoute party{CHAT_MSG_PARTY, 0, "", 17, 0};
    changed = party;
    changed.subgroup = 1;
    EXPECT_NE(party, changed);
    EXPECT_TRUE(Alles::IsRemoteSpeech(CHAT_MSG_PARTY_LEADER));
    EXPECT_TRUE(Alles::IsRemoteSpeech(CHAT_MSG_WHISPER));
    EXPECT_FALSE(Alles::IsRemoteSpeech(CHAT_MSG_WHISPER_INFORM));
    EXPECT_FALSE(Alles::NormalChatDeliveryPending());
}
