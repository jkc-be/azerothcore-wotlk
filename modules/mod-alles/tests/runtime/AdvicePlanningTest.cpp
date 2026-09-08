/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "runtime/AdvicePlanning.h"
#include "storage/PlanningCodec.h"
#include "gtest/gtest.h"
#include <boost/json.hpp>

namespace Alles
{
namespace
{
class AllesAdvicePlanningTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(knowledge.Seed(1, false, true));
        auto const* objective = book.ProposePlace(9, "Find work", "Search locally");
        ASSERT_NE(objective, nullptr);
        id = objective->id;
        ASSERT_TRUE(book.ActivatePlace(id, objective->revision, 1000, 5));
        ASSERT_TRUE(book.Block(id, Obstruction::Information, "No local work", 2000));
        ASSERT_TRUE(book.Defer(id, 2000));
        ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, "Where else can I find work?", 3000));
        reply = {{owner, 7, id, book.Find(id)->revision, 1, {Activity::Work, 0, 9, {}},
            book.Find(id)->information.question}, {ActorKey{ActorKind::Player, 2}, "Traveler"},
            "I heard there might be work in Westfall.", "General", 3002, 3002};
        ASSERT_TRUE(book.QuestionDelivery(id, 1, true, 3001));
    }

    AdviceJob Job()
    {
        auto job = PrepareAdvice(owner, 7, book, knowledge, reply, dictionary, 3010);
        EXPECT_TRUE(job);
        return job.value();
    }

    void Recruit()
    {
        book = ObjectiveBook{};
        QuestProgress progress;
        progress.inLog = true;
        progress.counters[0] = 2;
        auto const* objective = book.ProposeQuest(7, "Earn the reward for Group work", "Accepted quest", progress);
        ASSERT_NE(objective, nullptr);
        id = objective->id;
        ASSERT_TRUE(book.Block(id, Obstruction::Companions, "Needs a party", 2000));
        ASSERT_TRUE(book.Defer(id, 2000));
        std::string const question = "Who will help with Group work? Let's meet in Northshire Valley.";
        Cooperation cooperation;
        ASSERT_TRUE(BeginRecruitment(cooperation, owner, id, 7, 9, {{owner, "Recruiter"}, question, 3000}, 3000));
        ASSERT_TRUE(book.SetCooperation(id, objective->revision, cooperation));
        ASSERT_TRUE(book.Ask(id, objective->revision, question, 3000));
        reply.question = {owner, 7, id, objective->revision, 1, {Activity::Companions, 7, 9, {}},
            question, "Group work", "Northshire Valley"};
        reply.text = "I can join you for Group work. Invite me.";
        ASSERT_TRUE(book.QuestionDelivery(id, 1, true, 3001));
    }

    Bridge::PlanningDecision Invite(AdviceJob const& job)
    {
        return Bridge::DecodePlanningDecision({{"version", 1}, {"capability", "invite_companion"}, {"quest", 7},
            {"place", 0}, {"person", boost::json::object{{"kind", 0}, {"id", 2}}}, {"evidence", "reply"},
            {"reason", "The actual speaker offered to join this quest."}}, job.issued);
    }

    Bridge::PlanningDecision Decision(AdviceJob const& job, std::string capability, uint32_t place = 0)
    {
        return Bridge::DecodePlanningDecision({{"version", 1}, {"capability", capability}, {"quest", 0},
            {"place", place}, {"person", nullptr}, {"evidence", capability == "none" ? "" : "reply"},
            {"reason", "A supplied line supports this choice."}}, job.issued);
    }

    ActorKey owner{ActorKind::Player, 1};
    ObjectiveBook book;
    PrivateKnowledge knowledge;
    InformationReply reply;
    uint64_t id = 0;
    std::map<uint32_t, std::string> dictionary{{9, "Northshire Valley"}, {12, "Elwynn Forest"},
        {40, "Westfall"}, {99, "Secret place"}};
};
}

TEST_F(AllesAdvicePlanningTest, OnlyHeardNamesLeaveTheDictionaryAndAmbiguousUnknownNamesStayUnknown)
{
    dictionary[400] = "Westfall";
    EXPECT_TRUE(Job().heardPlaces.empty());
    dictionary.erase(400);
    auto job = Job();
    EXPECT_EQ(job.heardPlaces, (std::map<uint32_t, std::string>{{40, "Westfall"}}));
    EXPECT_EQ(job.issued.revision, book.Find(id)->revision);
    EXPECT_NE(job.issued.revision, reply.question.revision);
    auto serialized = boost::json::serialize(job.context);
    EXPECT_EQ(serialized.find("Secret place"), std::string::npos);
    EXPECT_EQ(serialized.find("Elwynn Forest"), std::string::npos);
    EXPECT_LT(serialized.size(), 8192u);
    reply.text = "Westfalling is unrelated. I do not know.";
    EXPECT_TRUE(Job().heardPlaces.empty());
    EXPECT_FALSE(knowledge.Places().contains(40));
}

TEST_F(AllesAdvicePlanningTest, RecruitmentRecordsOnlyTheActualOfferWithoutInventingMembershipOrCredit)
{
    Recruit();
    auto job = Job();
    EXPECT_EQ(job.context.at("purpose").as_string(), "recruitment");
    EXPECT_EQ(job.context.at("capabilities").as_array().size(), 2u);
    EXPECT_EQ(job.issued.people, (std::set<ActorKey>{{ActorKind::Player, 2}}));
    EXPECT_EQ(job.issued.quests, std::set<uint32_t>{7});
    EXPECT_TRUE(job.issued.places.empty());
    auto const progress = book.Find(id)->checkpoint;
    auto const beforeKnowledge = knowledge.Capture();
    auto outcome = ApplyAdvice(job, Invite(job), job.issued, book, knowledge, 3011);
    EXPECT_EQ(outcome.status, "companion_agreed");
    EXPECT_EQ(outcome.invite, reply.source.actor);
    auto const* objective = book.Find(id);
    EXPECT_EQ(objective->checkpoint, progress);
    EXPECT_EQ(objective->gainedCredit, 0u);
    EXPECT_EQ(objective->state, ObjectiveState::Deferred);
    EXPECT_EQ(objective->cooperation.state, CooperationState::Agreed);
    EXPECT_EQ(objective->cooperation.agreements.at(*reply.source.actor).statement, reply.text);
    EXPECT_EQ(knowledge.Capture(), beforeKnowledge);
    EXPECT_FALSE(book.Retryable(*objective, 3012, 99));
    PlanningSnapshot snapshot{owner, 1, book.Capture(), knowledge.Capture()};
    EXPECT_EQ(Storage::DecodePlanning(Storage::EncodePlanning(snapshot), owner), snapshot);
    EXPECT_FALSE(PrepareAdvice(owner, 7, book, knowledge, reply, dictionary, 3012));
    EXPECT_FALSE(ApplyAdvice(job, Invite(job), job.issued, book, knowledge, 3012).invite);
}

TEST_F(AllesAdvicePlanningTest, RecruitmentRejectsStaleActorsWrongReferencesAndExpiredOffersAtomically)
{
    Recruit();
    auto job = Job();
    for (unsigned scenario = 0; scenario < 8; ++scenario)
    {
        auto live = job.issued;
        auto decision = Invite(job);
        uint64_t now = 3011;
        switch (scenario)
        {
            case 0: ++live.actorGeneration; break;
            case 1: ++live.revision; break;
            case 2: live.autonomous = false; break;
            case 3: decision.request.person->id = 88; break;
            case 4: decision.request.quest = 8; break;
            case 5: decision.evidence = "invented"; break;
            case 6: now = 123000; break;
            case 7: decision.request.capability = "retry_quest"; decision.request.person.reset(); break;
        }
        auto before = book.Capture();
        EXPECT_FALSE(ApplyAdvice(job, decision, live, book, knowledge, now).invite);
        EXPECT_EQ(book.Capture(), before);
    }
    for (unsigned scenario = 0; scenario < 5; ++scenario)
    {
        auto changed = reply;
        switch (scenario)
        {
            case 0: changed.source.actor.reset(); break;
            case 1: changed.source.actor->kind = ActorKind::CreatureSpawn; break;
            case 2: changed.question.topic.place = 12; break;
            case 3: changed.question.placeName = "Secret place"; break;
            case 4: changed.gameMs = 3000; break;
        }
        EXPECT_FALSE(PrepareAdvice(owner, 7, book, knowledge, changed, dictionary, 3011));
    }
}

TEST_F(AllesAdvicePlanningTest, DeclinedOrAmbiguousRecruitmentReplyDoesNotRecordAgreement)
{
    Recruit();
    for (auto const* text : {"I cannot join.", "Maybe later.", "Someone in Westfall might help.", "What quest?"})
    {
        reply.text = text;
        auto job = Job();
        auto before = book.Capture();
        EXPECT_EQ(ApplyAdvice(job, Decision(job, "none"), job.issued, book, knowledge, 3011).status, "irrelevant");
        EXPECT_EQ(book.Capture(), before);
        EXPECT_TRUE(job.heardPlaces.empty());
    }
}

TEST_F(AllesAdvicePlanningTest, HelpOfferUsesOwnEligibilityAndRetainsOnlyTheDeliveredDeclaration)
{
    Recruit();
    ActorKey const helper{ActorKind::Player, 2};
    ObjectiveBook ownBook;
    PrivateKnowledge ownKnowledge;
    ASSERT_TRUE(ownKnowledge.Seed(2, false, true));
    ASSERT_FALSE(ownKnowledge.Places().contains(9));
    RecruitmentNotice notice{reply.question, {owner, "Recruiter"}, 3000};
    QuestProgress progress;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, false, 3010));
    ASSERT_TRUE(CanOfferHelp(helper, ownBook, notice, progress, true, 3010));
    auto intention = RecordHelpOffer(helper, "Helper", ownBook, ownKnowledge, notice, progress, true,
        "I can join you for Group work. Invite me.", 3010);
    ASSERT_TRUE(intention);
    auto const* objective = ownBook.Find(*intention);
    ASSERT_NE(objective, nullptr);
    EXPECT_EQ(objective->checkpoint, progress);
    EXPECT_FALSE(objective->checkpoint.inLog);
    EXPECT_EQ(objective->cooperation.state, CooperationState::Agreed);
    EXPECT_EQ(objective->cooperation.leader, owner);
    EXPECT_EQ(objective->cooperation.agreements.at(owner).statement, notice.question.text);
    EXPECT_EQ(ownKnowledge.Places().at(9).origin, KnowledgeOrigin::Reported);
    EXPECT_EQ(ownKnowledge.Places().at(9).visitedMs, 0u);
    ASSERT_EQ(ownKnowledge.Reports().size(), 1u);
    EXPECT_EQ(ownKnowledge.Reports().begin()->second.source, notice.source);
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, true, 3011));
    ASSERT_TRUE(ownBook.Reconcile(*intention, progress, 4000));
    EXPECT_EQ(objective->state, ObjectiveState::Deferred);
    EXPECT_FALSE(ownBook.Retryable(*objective, 4000, 99));
    PlanningSnapshot snapshot{helper, 1, ownBook.Capture(), ownKnowledge.Capture()};
    EXPECT_EQ(Storage::DecodePlanning(Storage::EncodePlanning(snapshot), helper), snapshot);
    progress.inLog = true;
    ASSERT_TRUE(ownBook.Observe(*intention, progress, ObjectiveStep::Wait, 5000));
    EXPECT_EQ(objective->gainedCredit, 0u);
    progress.inLog = false;
    ASSERT_TRUE(ownBook.Observe(*intention, progress, ObjectiveStep::Wait, 6000));
    EXPECT_EQ(objective->state, ObjectiveState::Cancelled); // Real later abandonment is not a pending share.
}

TEST_F(AllesAdvicePlanningTest, OffersPreserveConflictingWorkAndFailureLeavesNoPartialKnowledge)
{
    Recruit();
    ActorKey const helper{ActorKind::Player, 2};
    RecruitmentNotice notice{reply.question, {owner, "Recruiter"}, 3000};
    ObjectiveBook ownBook;
    PrivateKnowledge ownKnowledge;
    QuestProgress progress;
    progress.inLog = true;
    auto const* current = ownBook.ProposeQuest(8, "Finish current work", "Accepted quest", progress);
    ASSERT_TRUE(ownBook.Activate(current->id, current->revision, progress, 3001, 5));
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, true, 3010));
    ownBook = ObjectiveBook{};
    auto const before = ownBook.Capture();
    auto const knowledgeBefore = ownKnowledge.Capture();
    EXPECT_FALSE(RecordHelpOffer(helper, "Helper", ownBook, ownKnowledge, notice, progress, true, "", 3010));
    EXPECT_EQ(ownBook.Capture(), before);
    EXPECT_EQ(ownKnowledge.Capture(), knowledgeBefore);
    EXPECT_TRUE(CanOfferHelp(helper, ownBook, notice, progress, true, 33000));
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, true, 123000));
    notice.question.partySize = 6;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, true, 3010));
    notice.question.partySize = 1;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, true, 3010));
    notice.question.partySize = 2;
    EXPECT_FALSE(CanOfferHelp(owner, ownBook, notice, progress, true, 3010));
    progress.readyToReward = true;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, progress, true, 3010));
    progress.readyToReward = false;
    progress.rewarded = true;
    EXPECT_TRUE(CanOfferHelp(helper, ownBook, notice, progress, false, 3010));
}

TEST_F(AllesAdvicePlanningTest, UnsharedAgreementExpiresWithoutInventingQuestAcceptance)
{
    Recruit();
    ActorKey const helper{ActorKind::Player, 2};
    RecruitmentNotice notice{reply.question, {owner, "Recruiter"}, 3000};
    ObjectiveBook ownBook;
    PrivateKnowledge ownKnowledge;
    auto id = RecordHelpOffer(helper, "Helper", ownBook, ownKnowledge, notice, {}, true, "I can help.", 3010);
    ASSERT_TRUE(id);
    auto const* objective = ownBook.Find(*id);
    ASSERT_TRUE(ownBook.Reconcile(*id, {}, objective->cooperation.deadlineMs));
    EXPECT_EQ(objective->state, ObjectiveState::Deferred);
    EXPECT_EQ(objective->cooperation.state, CooperationState::Deferred);
    EXPECT_FALSE(objective->checkpoint.inLog);
    EXPECT_EQ(objective->gainedCredit, 0u);
    auto const firstRetry = objective->cooperation.reconsiderMs;
    notice.receivedMs = firstRetry - 1;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, {}, true, firstRetry - 1));
    auto retried = RecordHelpOffer(helper, "Helper", ownBook, ownKnowledge, notice, {}, true,
        "I can try to join again.", firstRetry);
    ASSERT_EQ(retried, id);
    objective = ownBook.Find(*id);
    EXPECT_EQ(ownBook.All().size(), 1u);
    EXPECT_EQ(objective->cooperation.attempts, 2u);
    EXPECT_EQ(objective->attempts, 0u); // An unshared quest was never actively attempted.
    EXPECT_FALSE(objective->checkpoint.inLog);
    ASSERT_TRUE(ownBook.Reconcile(*id, {}, objective->cooperation.deadlineMs));
    auto const secondRetry = objective->cooperation.reconsiderMs;
    notice.receivedMs = secondRetry;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, {}, true, secondRetry));
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(ownBook.Capture()));
    EXPECT_FALSE(CanOfferHelp(helper, restored, notice, {}, true, secondRetry));
}

TEST_F(AllesAdvicePlanningTest, RewardedHelperRetainsOwnCompletionWhileWaitingForPeersRealProgress)
{
    Recruit();
    ActorKey const helper{ActorKind::Player, 2};
    RecruitmentNotice notice{reply.question, {owner, "Recruiter"}, 3000};
    for (bool retained : {false, true})
    {
        ObjectiveBook ownBook;
        PrivateKnowledge ownKnowledge;
        QuestProgress rewarded;
        rewarded.rewarded = true;
        rewarded.counters[0] = 4;
        if (retained)
        {
            auto const* prior = ownBook.ProposeQuest(7, "Earn my own reward", "Previously accepted", rewarded);
            ASSERT_NE(prior, nullptr);
            ASSERT_TRUE(ownBook.Observe(prior->id, rewarded, ObjectiveStep::Wait, 2500));
        }
        auto id = RecordHelpOffer(helper, "Helper", ownBook, ownKnowledge, notice, rewarded, false,
            "I already finished, but I can help you with Group work.", 3010);
        ASSERT_TRUE(id);
        auto const* objective = ownBook.Find(*id);
        EXPECT_EQ(ownBook.All().size(), 1u);
        EXPECT_EQ(objective->state, ObjectiveState::Completed);
        EXPECT_EQ(objective->checkpoint, rewarded);
        EXPECT_EQ(objective->attempts, 0u);
        EXPECT_EQ(objective->gainedCredit, 0u);
        auto cooperation = objective->cooperation;
        EXPECT_EQ(cooperation.state, CooperationState::Agreed);
        PartyObservation party{99, owner, true, false,
            {{owner, true, true, true, true, true, false}, {helper, true, true, true, true, true, true}}};
        ASSERT_TRUE(ObserveCooperation(cooperation, party, 4000));
        EXPECT_EQ(cooperation.state, CooperationState::Working);
        EXPECT_EQ(CooperativeGuide(cooperation, party), owner);
        ASSERT_TRUE(ownBook.SetCooperation(*id, objective->revision, cooperation));
        PlanningSnapshot snapshot{helper, 1, ownBook.Capture(), ownKnowledge.Capture()};
        EXPECT_EQ(Storage::DecodePlanning(Storage::EncodePlanning(snapshot), helper), snapshot);
        party.members[0].finished = true;
        ASSERT_TRUE(ObserveCooperation(cooperation, party, 5000));
        EXPECT_EQ(cooperation.state, CooperationState::Completed);
    }
}

TEST_F(AllesAdvicePlanningTest, RealAbandonmentCannotBecomeAnUnsharedOfferRetry)
{
    Recruit();
    ActorKey const helper{ActorKind::Player, 2};
    RecruitmentNotice notice{reply.question, {owner, "Recruiter"}, 3000};
    ObjectiveBook ownBook;
    PrivateKnowledge ownKnowledge;
    QuestProgress progress;
    progress.inLog = true;
    auto id = RecordHelpOffer(helper, "Helper", ownBook, ownKnowledge, notice, progress, true,
        "I can help.", 3010);
    ASSERT_TRUE(id);
    ASSERT_TRUE(ownBook.Observe(*id, {}, ObjectiveStep::Wait, 4000));
    EXPECT_EQ(ownBook.Find(*id)->state, ObjectiveState::Cancelled);
    auto cooperation = ownBook.Find(*id)->cooperation;
    EXPECT_EQ(cooperation.state, CooperationState::Cancelled);
    EXPECT_FALSE(DeferCooperation(cooperation, "Party did not form", 5000));
    notice.receivedMs = cooperation.deadlineMs + 600000;
    EXPECT_FALSE(CanOfferHelp(helper, ownBook, notice, {}, true, notice.receivedMs));
}

TEST_F(AllesAdvicePlanningTest, WarningRetainsExactWordsWithoutReopeningOrCompletingTheObjective)
{
    reply.text = "Avoid Westfall; I found no work there yesterday.";
    auto job = Job();
    auto before = book.Capture();
    auto outcome = ApplyAdvice(job, Decision(job, "remember_place_report", 40), job.issued, book, knowledge, 3011);
    EXPECT_EQ(outcome.status, "report_retained");
    ASSERT_NE(outcome.report, 0u);
    EXPECT_EQ(outcome.intention, 0u);
    EXPECT_EQ(book.Capture(), before);
    auto const& report = knowledge.Reports().at(outcome.report);
    EXPECT_EQ(report.source, reply.source);
    EXPECT_EQ(report.text, reply.text);
    EXPECT_EQ(report.receivedMs, reply.gameMs);
    EXPECT_DOUBLE_EQ(report.confidence, 0.4);
    EXPECT_EQ(report.usefulVisits, 0u);
    auto const& place = knowledge.Places().at(40);
    EXPECT_EQ(place.origin, KnowledgeOrigin::Reported);
    EXPECT_EQ(place.minimumLevel, 0u);
    EXPECT_EQ(place.maximumLevel, 0u);
    EXPECT_EQ(place.visitedMs, 0u);
    EXPECT_EQ(place.lastUsefulWorkMs, 0u);
    EXPECT_EQ(place.relativeTo, 0u);
    EXPECT_TRUE(place.direction.empty());
    auto knowledgeBefore = knowledge.Capture();
    EXPECT_EQ(ApplyAdvice(job, Decision(job, "remember_place_report", 40), job.issued,
        book, knowledge, 3012).report, outcome.report);
    EXPECT_EQ(knowledge.Capture(), knowledgeBefore);
}

TEST_F(AllesAdvicePlanningTest, UsableLeadQueuesInvestigationRetainsProvenanceAndRequiresObservedNewWork)
{
    auto job = Job();
    auto outcome = ApplyAdvice(job, Decision(job, "investigate_report", 40), job.issued, book, knowledge, 3011);
    ASSERT_EQ(outcome.status, "lead_applied");
    ASSERT_NE(outcome.intention, 0u);
    auto const* parent = book.Find(id);
    EXPECT_EQ(parent->state, ObjectiveState::Deferred);
    EXPECT_EQ(parent->information.status, InformationStatus::Lead);
    EXPECT_GT(parent->nextReconsiderationMs, 3011u);
    auto const* child = book.Find(outcome.intention);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->parent, id);
    EXPECT_EQ(child->evidence, std::vector<uint64_t>{outcome.report});
    EXPECT_EQ(child->state, ObjectiveState::Proposed);
    EXPECT_EQ(child->place, 40u);
    EXPECT_EQ(child->discoveredQuest, 0u);
    EXPECT_EQ(child->arrivedMs, 0u);
    EXPECT_FALSE(child->checkpoint.rewarded);
    PlanningSnapshot snapshot{owner, 1, book.Capture(), knowledge.Capture()};
    auto saved = Storage::DecodePlanning(Storage::EncodePlanning(snapshot), owner);
    ASSERT_TRUE(saved);
    EXPECT_EQ(*saved, snapshot);
    PrivateKnowledge otherOwner;
    ASSERT_TRUE(otherOwner.Seed(1, false, true));
    EXPECT_FALSE(otherOwner.Places().contains(40));
    EXPECT_TRUE(otherOwner.Reports().empty());
    ASSERT_TRUE(book.ActivatePlace(child->id, child->revision, 4000, 5));
    ASSERT_TRUE(book.ObservePlace(child->id, 40, 0, ObjectiveStep::Attempt, 5000));
    EXPECT_NE(child->state, ObjectiveState::Completed);
    ASSERT_TRUE(knowledge.Visit(40, "Westfall", 5000, false));
    EXPECT_FALSE(knowledge.Assess(outcome.report, 40, true));
    ASSERT_TRUE(book.ObservePlace(child->id, 40, 123, ObjectiveStep::Attempt, 6000));
    EXPECT_EQ(child->state, ObjectiveState::Completed);
    ASSERT_TRUE(knowledge.RecordUsefulWork(40, 6000, 5));
    EXPECT_TRUE(knowledge.Assess(outcome.report, 40, true));
    EXPECT_EQ(knowledge.Places().at(40).minimumLevel, 5u);
    EXPECT_EQ(knowledge.Reports().at(outcome.report).usefulVisits, 1u);
    EXPECT_DOUBLE_EQ(knowledge.Reports().at(outcome.report).confidence, 0.4);
}

TEST_F(AllesAdvicePlanningTest, StaleActorHandoffExpiryAndUnsupportedReferencesApplyNothing)
{
    auto job = Job();
    for (unsigned scenario = 0; scenario < 6; ++scenario)
    {
        auto live = job.issued;
        auto decision = Decision(job, "investigate_report", 40);
        auto now = uint64_t(3011);
        switch (scenario)
        {
            case 0: ++live.actorGeneration; break;
            case 1: ++live.revision; break;
            case 2: live.autonomous = false; break;
            case 3: now = 123000; break;
            case 4: decision.request.place = 99; break;
            case 5: decision.evidence = "invented"; break;
        }
        auto beforeBook = book.Capture();
        auto beforeKnowledge = knowledge.Capture();
        EXPECT_EQ(ApplyAdvice(job, decision, live, book, knowledge, now).report, 0u);
        EXPECT_EQ(book.Capture(), beforeBook);
        EXPECT_EQ(knowledge.Capture(), beforeKnowledge);
    }
    auto foreign = reply;
    foreign.question.owner.id = 88;
    EXPECT_FALSE(PrepareAdvice(owner, 7, book, knowledge, foreign, dictionary, 3011));
}

TEST_F(AllesAdvicePlanningTest, UnrelatedChatterAndUnavailableLeadLeaveNoPartialReportOrGeography)
{
    auto job = Job();
    auto before = knowledge.Capture();
    EXPECT_EQ(ApplyAdvice(job, Decision(job, "none"), job.issued, book, knowledge, 3011).status, "irrelevant");
    EXPECT_EQ(knowledge.Capture(), before);
    reply.text = "Look in Northshire Valley again.";
    job = Job();
    auto bookBefore = book.Capture();
    EXPECT_EQ(ApplyAdvice(job, Decision(job, "investigate_report", 9), job.issued,
        book, knowledge, 3011).status, "lead_not_applicable");
    EXPECT_EQ(book.Capture(), bookBefore);
    EXPECT_EQ(knowledge.Capture(), before);
}
TEST_F(AllesAdvicePlanningTest, LearningPreservesCurrentExecutionAndCapacityFailureIsAtomic)
{
    QuestProgress progress;
    progress.inLog = true;
    auto const* current = book.ProposeQuest(7, "Finish my current work", "An accepted quest");
    ASSERT_NE(current, nullptr);
    auto const currentId = current->id;
    ASSERT_TRUE(book.Activate(currentId, current->revision, progress, 3005, 5));
    auto job = Job();
    ASSERT_TRUE(book.Observe(currentId, progress, ObjectiveStep::Attempt, 3009));
    auto beforeCurrent = *book.Current();
    auto outcome = ApplyAdvice(job, Decision(job, "investigate_report", 40), job.issued, book, knowledge, 3011);
    ASSERT_EQ(outcome.status, "lead_applied");
    ASSERT_NE(book.Current(), nullptr);
    EXPECT_EQ(*book.Current(), beforeCurrent);
    EXPECT_NE(book.Current()->lastSampleMs, 0u);
}

TEST_F(AllesAdvicePlanningTest, FullObjectiveBookCannotLeaveAnOrphanReportOrNewPlace)
{
    ObjectivePolicy policy;
    policy.maxObjectives = 1;
    ObjectiveBook limited(policy);
    ASSERT_TRUE(limited.Restore(book.Capture()));
    auto job = Job();
    auto beforeBook = limited.Capture();
    auto beforeKnowledge = knowledge.Capture();
    EXPECT_EQ(ApplyAdvice(job, Decision(job, "investigate_report", 40), job.issued,
        limited, knowledge, 3011).status, "lead_not_applicable");
    EXPECT_EQ(limited.Capture(), beforeBook);
    EXPECT_EQ(knowledge.Capture(), beforeKnowledge);
}

TEST_F(AllesAdvicePlanningTest, ConcreteQuestAdviceReopensRetryWithoutInventingCreditOrRefreshingRepeatedReports)
{
    book = ObjectiveBook{};
    auto const* objective = book.ProposeQuest(7, "Earn the quest reward", "My accepted quest");
    ASSERT_NE(objective, nullptr);
    id = objective->id;
    QuestProgress progress;
    progress.inLog = true;
    progress.counters[0] = 2;
    ASSERT_TRUE(book.Activate(id, objective->revision, progress, 1000, 5));
    ASSERT_TRUE(book.Block(id, Obstruction::Information, "Missing target location", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, "Where should I look for my quest target?", 3000));
    reply.question = {owner, 7, id, book.Find(id)->revision, 1, {Activity::Work, 7, 0, {}},
        book.Find(id)->information.question};
    reply.text = "I saw them on the hill beside the camp, away from the road.";
    ASSERT_TRUE(book.QuestionDelivery(id, 1, true, 3001));
    auto job = Job();
    auto decision = Decision(job, "retry_quest");
    decision.request.quest = 7;
    auto outcome = ApplyAdvice(job, decision, job.issued, book, knowledge, 3011);
    ASSERT_EQ(outcome.status, "lead_applied");
    EXPECT_EQ(outcome.intention, id);
    EXPECT_EQ(book.Find(id)->checkpoint, progress);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
    EXPECT_TRUE(book.Retryable(*book.Find(id), 3011, 5));
    ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, reply.question.text, 603000));
    reply.question.attempt = 2;
    reply.question.revision = book.Find(id)->revision;
    reply.gameMs = 603002;
    ASSERT_TRUE(book.QuestionDelivery(id, 2, true, 603001));
    auto repeated = PrepareAdvice(owner, 7, book, knowledge, reply, dictionary, 603010);
    ASSERT_TRUE(repeated);
    decision = Decision(*repeated, "retry_quest");
    decision.request.quest = 7;
    auto before = book.Capture();
    EXPECT_EQ(ApplyAdvice(*repeated, decision, repeated->issued, book, knowledge, 603011).status,
        "lead_not_applicable");
    EXPECT_EQ(book.Capture(), before);
    EXPECT_EQ(knowledge.Reports().at(outcome.report).receivedMs, 3002u);
}

}
