/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "runtime/ConversationPolicy.h"
#include "runtime/ConversationKnowledge.h"
#include "runtime/ConversationRouting.h"
#include "SharedDefines.h"
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include "gtest/gtest.h"

namespace
{
using namespace Alles;

TEST(AllesConversationPolicyTest, ReversedRepliesRetainOnlyThatPairsDialogueOnTheSameRoute)
{
    SpeechRoute publicRoute{CHAT_MSG_CHANNEL, 1, "General - Elwynn Forest"};
    SpeechRoute privateRoute{CHAT_MSG_WHISPER};
    EXPECT_EQ(SharedDialogueKey(1, 2, publicRoute), SharedDialogueKey(2, 1, publicRoute));
    EXPECT_NE(SharedDialogueKey(1, 2, publicRoute), SharedDialogueKey(2, 1, privateRoute));
    EXPECT_NE(SharedDialogueKey(1, 2, publicRoute), SharedDialogueKey(1, 3, publicRoute));
    auto elsewhere = publicRoute;
    elsewhere.channelName = "General - Dun Morogh";
    EXPECT_NE(SharedDialogueKey(1, 2, publicRoute), SharedDialogueKey(2, 1, elsewhere));
}

TEST(AllesConversationPolicyTest, RelevantRespondentWinsAndMissingNamedListenerDoesNotRedirect)
{
    std::vector<ReplyCandidate> candidates{{7, 1}, {8, 8}, {9, 6}};
    EXPECT_EQ(ElectRespondent(candidates, std::nullopt, 0), 8u);
    EXPECT_EQ(ElectRespondent(candidates, 7, 0), 7u);
    EXPECT_FALSE(ElectRespondent(candidates, 10, 0));
    EXPECT_FALSE(ElectRespondent({{7, 0}}, std::nullopt, 0));
    EXPECT_EQ(ElectRespondent({{7, 4}, {8, 4}}, std::nullopt, 0), 7u);
    EXPECT_EQ(ElectRespondent({{8, 4}, {7, 4}}, std::nullopt, 1), 8u);
}

TEST(AllesConversationPolicyTest, OnePendingReplyTwoParticipantsAndFourAttemptsBoundAnExchange)
{
    ConversationPolicy policy;
    auto thread = policy.Open(1, "general:elwynn", "Where can I find work?", 1000);
    ASSERT_TRUE(thread);
    EXPECT_TRUE(policy.Reserve(*thread, 1, 2, 1001));
    EXPECT_FALSE(policy.Reserve(*thread, 1, 3, 1002));
    EXPECT_FALSE(policy.Reserve(*thread, 2, 1, 1002));
    policy.Finish(*thread);
    EXPECT_FALSE(policy.Reserve(*thread, 1, 3, 1003));
    EXPECT_FALSE(policy.Reserve(*thread, 3, 1, 1003));
    EXPECT_TRUE(policy.Reserve(*thread, 2, 1, 1003));
    policy.Finish(*thread);
    EXPECT_TRUE(policy.Reserve(*thread, 1, 2, 1004));
    policy.Finish(*thread);
    EXPECT_TRUE(policy.Reserve(*thread, 2, 1, 1005));
    policy.Finish(*thread);
    EXPECT_FALSE(policy.CanReply(*thread, 1, 2, 1006));
    EXPECT_FALSE(policy.CanReply(*thread, 2, 1, 121000));
}

TEST(AllesConversationPolicyTest, FailedJobsSpendBudgetAndRelogDoesNotRefundIt)
{
    ConversationPolicy policy;
    for (unsigned i = 0; i < 4; ++i)
    {
        auto thread = policy.Open(1, "whisper:1:2", "question " + std::to_string(i), 1000 + i);
        ASSERT_TRUE(thread);
        EXPECT_TRUE(policy.Reserve(*thread, 1, 2, 1000 + i));
        policy.Finish(*thread);
    }
    policy.Forget(2);
    auto thread = policy.Open(1, "another-audience", "different topic", 1005);
    ASSERT_TRUE(thread);
    EXPECT_FALSE(policy.Reserve(*thread, 1, 2, 1005));
    EXPECT_TRUE(policy.Reserve(*thread, 1, 2, 61005));
}

TEST(AllesConversationPolicyTest, ChangingBotsDoesNotBypassTheAudienceBudget)
{
    ConversationPolicy policy;
    for (unsigned i = 0; i < 6; ++i)
    {
        auto thread = policy.Open(1, "general:elwynn", "question " + std::to_string(i), 1000);
        ASSERT_TRUE(thread);
        EXPECT_TRUE(policy.Reserve(*thread, 1, 10 + i, 1001));
        policy.Finish(*thread);
    }
    auto thread = policy.Open(1, "general:elwynn", "another question", 1002);
    ASSERT_TRUE(thread);
    EXPECT_FALSE(policy.Reserve(*thread, 1, 99, 1002));
    auto elsewhere = policy.Open(1, "general:dunmorogh", "another question", 1002);
    ASSERT_TRUE(elsewhere);
    EXPECT_TRUE(policy.Reserve(*elsewhere, 1, 99, 1002));
}

TEST(AllesConversationPolicyTest, RepeatTopicsAndDeliveredAnswersExpireWithoutCrossingAudiences)
{
    ConversationPolicy policy;
    auto thread = policy.Open(1, "general:elwynn", "Where is work?", 1000);
    ASSERT_TRUE(thread);
    EXPECT_FALSE(policy.Open(3, "general:elwynn", "WHERE is work!!!", 2000));
    EXPECT_TRUE(policy.Open(3, "whisper:1:3", "WHERE is work!!!", 2000));
    EXPECT_FALSE(policy.DuplicateAnswer(*thread, "Try the village southwest of this valley", 2000));
    policy.Delivered(*thread, "Try the village southwest of this valley", 2000);
    EXPECT_TRUE(policy.DuplicateAnswer(*thread, "Try the village southwest of this valley!", 3000));
    EXPECT_TRUE(policy.DuplicateAnswer(*thread, "Try the village southwest of the valley", 3000));
    EXPECT_FALSE(policy.DuplicateAnswer(*thread, "Try the snowy country northeast of the pass", 3000));
    EXPECT_FALSE(policy.DuplicateAnswer(*thread, "Try the village southwest of this valley", 62000));
    EXPECT_TRUE(policy.Open(3, "general:elwynn", "Where is work?", 121000));
}

TEST(AllesConversationPolicyTest, UnansweredThreadsAndMalformedInputsAreBounded)
{
    ConversationPolicy policy;
    EXPECT_FALSE(policy.Open(0, "general", "hello", 1000));
    EXPECT_FALSE(policy.Open(1, "", "hello", 1000));
    EXPECT_FALSE(policy.Open(1, "general", std::string(256, 'x'), 1000));
    for (unsigned i = 0; i < 128; ++i)
        ASSERT_TRUE(policy.Open(1, "general", "question " + std::to_string(i), 1000));
    EXPECT_FALSE(policy.Open(1, "general", "overflow", 1000));
    policy.Prune(121000);
    EXPECT_EQ(policy.Size(), 0u);
    EXPECT_TRUE(policy.Open(1, "general", "new work", 121000));
}

TEST(AllesConversationPolicyTest, InitiatedQuestionsShareTheSameAudienceAndActorBudgetsAsReplies)
{
    ConversationPolicy policy;
    auto thread = policy.Open(1, "general", "Where can I find work?", 1000);
    ASSERT_TRUE(thread);
    ASSERT_TRUE(policy.ReserveQuestion(*thread, 1000));
    EXPECT_FALSE(policy.ReserveQuestion(*thread, 1001));
    EXPECT_TRUE(policy.Reserve(*thread, 1, 2, 1001));
    policy.Finish(*thread);
    EXPECT_TRUE(policy.Reserve(*thread, 2, 1, 1002));
    policy.Finish(*thread);
    EXPECT_EQ(policy.Find(*thread)->attempts, 2u);
    for (unsigned i = 0; i < 2; ++i)
    {
        auto other = policy.Open(1, "other-audience", "another topic " + std::to_string(i), 1003 + i);
        ASSERT_TRUE(other);
        EXPECT_TRUE(policy.ReserveQuestion(*other, 1003 + i));
    }
    auto exhausted = policy.Open(1, "third-audience", "more work", 1005);
    ASSERT_TRUE(exhausted);
    EXPECT_FALSE(policy.ReserveQuestion(*exhausted, 1005));
    EXPECT_TRUE(policy.ReserveQuestion(*exhausted, 61005));
}

TEST(AllesConversationPolicyTest, RecruitmentSerializesDistinctOffersWithoutSuppressingIdenticalConsent)
{
    ConversationPolicy policy;
    auto thread = policy.Open(1, "general", "Seeking four companions", 1000);
    ASSERT_TRUE(thread);
    EXPECT_FALSE(policy.EnableRecruitment(*thread, 4));
    ASSERT_TRUE(policy.ReserveQuestion(*thread, 1000));
    EXPECT_FALSE(policy.EnableRecruitment(*thread, 0));
    EXPECT_FALSE(policy.EnableRecruitment(*thread, 5));
    ASSERT_TRUE(policy.EnableRecruitment(*thread, 4));
    EXPECT_FALSE(policy.EnableRecruitment(*thread, 1));
    for (uint64_t actor = 2; actor <= 5; ++actor)
    {
        ASSERT_TRUE(policy.Reserve(*thread, 1, actor, 1001));
        EXPECT_FALSE(policy.Reserve(*thread, 1, actor + 1, 1001));
        EXPECT_FALSE(policy.DuplicateAnswer(*thread, "I can help", 1001));
        policy.Delivered(*thread, "I can help", 1001);
        policy.Finish(*thread);
        EXPECT_FALSE(policy.CanReply(*thread, 1, actor, 1002));
        EXPECT_FALSE(policy.CanReply(*thread, actor, 1, 1002));
    }
    EXPECT_EQ(policy.Find(*thread)->attempts, 4u);
    EXPECT_FALSE(policy.CanReply(*thread, 1, 6, 1002));
    EXPECT_FALSE(policy.CanReply(*thread, 1, 6, 61002));
    policy.Forget(3);
    EXPECT_EQ(policy.Find(*thread), nullptr);
}

TEST(AllesConversationPolicyTest, FailedRecruitmentOffersSpendSharedBudgetsAndCannotBeRetriedOnTheThread)
{
    ConversationPolicy policy;
    for (unsigned i = 0; i < 3; ++i)
    {
        auto thread = policy.Open(1, "general", "Recruiting for quest " + std::to_string(i), 1000);
        ASSERT_TRUE(thread);
        ASSERT_TRUE(policy.ReserveQuestion(*thread, 1000));
        ASSERT_TRUE(policy.EnableRecruitment(*thread, 1));
        ASSERT_TRUE(policy.Reserve(*thread, 1, 2, 1000));
        policy.Finish(*thread); // No delivered answer: failed jobs still spend the slot.
        EXPECT_FALSE(policy.Reserve(*thread, 1, 2, 1001));
        EXPECT_FALSE(policy.Reserve(*thread, 1, 3, 1001));
    }
    policy.Forget(2);
    auto crowded = policy.Open(3, "general", "A fourth request", 1002);
    ASSERT_TRUE(crowded);
    EXPECT_FALSE(policy.ReserveQuestion(*crowded, 1002)); // Shared six-message audience limit, no refund.
    auto elsewhere = policy.Open(1, "whisper:1:3", "Another request", 1002);
    ASSERT_TRUE(elsewhere);
    ASSERT_TRUE(policy.ReserveQuestion(*elsewhere, 1002));
    auto exhausted = policy.Open(1, "whisper:1:4", "One more request", 1002);
    ASSERT_TRUE(exhausted);
    EXPECT_FALSE(policy.ReserveQuestion(*exhausted, 1002)); // Four questions across audiences.
    EXPECT_TRUE(policy.ReserveQuestion(*exhausted, 61002));
}

TEST(AllesConversationKnowledgeTest, ActivityRetrievalUsesOnlyTheGivenOwnersPrivateReportsAndGeography)
{
    PrivateKnowledge knowledge;
    ASSERT_TRUE(knowledge.Seed(1, false, true));
    ASSERT_TRUE(knowledge.Hear({ActorKey{ActorKind::Player, 2}, "Humanb"}, {Activity::Hunt, 0, 87},
        "I found useful prey outside the village", 1000, 0.4));
    OwnerSnapshot owner;
    owner.owner = {ActorKind::Player, 1};
    owner.planning = PlanningSnapshot{owner.owner, 1, {}, knowledge.Capture()};
    auto advice = RetrieveConversationKnowledge(owner, "Where can I hunt?", 5, 2000);
    EXPECT_FALSE(advice.places.empty());
    ASSERT_EQ(advice.reports.size(), 1u);
    EXPECT_EQ(advice.reports[0].as_object().at("source").as_string(), "Humanb");
    EXPECT_EQ(advice.reports[0].as_object().at("receivedMs").as_uint64(), 1000u);
    EXPECT_DOUBLE_EQ(advice.reports[0].as_object().at("confidence").as_double(), 0.4);
    EXPECT_EQ(advice.relevance, 6u);
    OwnerSnapshot stranger;
    stranger.owner = {ActorKind::Player, 3};
    auto unknown = RetrieveConversationKnowledge(stranger, "Where can I hunt?", 5, 2000);
    EXPECT_TRUE(unknown.places.empty());
    EXPECT_TRUE(unknown.reports.empty());
    EXPECT_EQ(unknown.relevance, 0u);
}

TEST(AllesConversationKnowledgeTest, PersonallyExperiencedHarmRemainsUsefulEvidenceForMatchingQuestions)
{
    OwnerSnapshot owner;
    owner.owner = {ActorKind::Player, 1};
    Memory death;
    death.kind = MemoryKind::OwnDeath;
    death.claim = "I died while looking for work";
    death.salience = 1;
    owner.memories.push_back(death);
    Memory lead;
    lead.kind = MemoryKind::HeardStatement;
    lead.claim = "There may be work outside the village";
    lead.source.name = "Humanb";
    lead.salience = 0.4;
    owner.memories.push_back(lead);
    auto advice = RetrieveConversationKnowledge(owner, "Where is work?", 5, 1000);
    ASSERT_EQ(advice.memories.size(), 2u);
    EXPECT_EQ(advice.memories[0].as_string(), RenderMemory(death));
    EXPECT_EQ(advice.memories[1].as_string(), RenderMemory(lead));
}
TEST(AllesConversationKnowledgeTest, GreetingsDoNotBecomeEvidenceJustBecauseTheNameMatches)
{
    OwnerSnapshot owner;
    Perception speech;
    speech.source.name = "Humana";
    speech.text = "Hello, Humanb. It is good to see you.";
    owner.memories.push_back(FormFallback(speech, {}, 100));
    speech.text = "Humana found work at the abbey.";
    owner.memories.push_back(FormFallback(speech, {}, 100));
    auto advice = RetrieveConversationKnowledge(owner, "What did Humana find?", 1, 1000);
    ASSERT_EQ(advice.memories.size(), 1u);
    EXPECT_EQ(advice.memories[0].as_string(), RenderMemory(owner.memories.back()));
}

TEST(AllesConversationKnowledgeTest, ContextBoundCountsEscapingAndKeepsTheActualQuestion)
{
    boost::json::object context{{"message", "Where can I hunt?"}, {"localActions", false},
        {"knownPlaces", boost::json::array{}}, {"learnedReports", boost::json::array{}},
        {"history", boost::json::array{}}};
    for (unsigned i = 0; i < 6; ++i)
        context.at("knownPlaces").as_array().emplace_back(std::string(400, 'p'));
    for (unsigned i = 0; i < 4; ++i)
        context.at("learnedReports").as_array().emplace_back(std::string(2048, '\"'));
    for (unsigned i = 0; i < 8; ++i)
        context.at("history").as_array().emplace_back(std::to_string(i) + std::string(254, 'h'));
    EXPECT_GT(boost::json::serialize(context).size(), 8000u);
    EXPECT_TRUE(BoundConversationContext(context));
    EXPECT_LE(boost::json::serialize(context).size(), 8000u);
    EXPECT_EQ(context.at("message").as_string(), "Where can I hunt?");
    EXPECT_FALSE(context.at("localActions").as_bool());
    EXPECT_FALSE(context.at("learnedReports").as_array().empty());
    EXPECT_FALSE(context.at("knownPlaces").as_array().empty());
    boost::json::object impossible{{"message", std::string(9000, 'x')}};
    EXPECT_FALSE(BoundConversationContext(impossible));
}

TEST(AllesConversationPolicyTest, DeliveredGreetingAcknowledgementEndsThreadWithoutBlockingNewQuestions)
{
    ConversationPolicy policy;
    auto thread = policy.Open(1, "nearby", "Hello, Humana. It is good to see you.", 1000);
    ASSERT_TRUE(thread);
    ASSERT_TRUE(policy.Reserve(*thread, 1, 2, 1001));
    policy.Finish(*thread); // A failed delivery cannot complete the exchange.
    EXPECT_TRUE(policy.CanReply(*thread, 1, 2, 1002));
    policy.Delivered(*thread, "Hello, Humanb. It's good to see you too.", 1003);
    EXPECT_FALSE(policy.CanReply(*thread, 2, 1, 1004));
    auto question = policy.Open(2, "nearby", "Can you help me with these wolves?", 1005);
    ASSERT_TRUE(question);
    EXPECT_TRUE(policy.CanReply(*question, 2, 1, 1006));
    auto substantive = policy.Open(3, "elsewhere", "Hello, Humana.", 1007);
    ASSERT_TRUE(substantive);
    policy.Delivered(*substantive, "Hello! Do you know a safe route to the village?", 1008);
    EXPECT_TRUE(policy.CanReply(*substantive, 3, 4, 1009));
}

}
