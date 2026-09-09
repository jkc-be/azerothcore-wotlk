/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "runtime/ObjectivePlanning.h"
#include "storage/PlanningCodec.h"
#include "gtest/gtest.h"
#include <boost/json.hpp>
#include <limits>

namespace Alles
{
namespace
{
class AllesObjectivePlanningTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(knowledge.Seed(1, false, true));
        progress.inLog = true;
        progress.counters[0] = 2;
        auto const* first = book.ProposeQuest(7, "Earn the first reward", "Accepted work", progress);
        ASSERT_NE(first, nullptr);
        currentId = first->id;
        ASSERT_EQ(first->gainedCredit, 0u);
        ASSERT_TRUE(book.Activate(currentId, first->revision, progress, 1000, 5));
        ASSERT_TRUE(book.Observe(currentId, progress, ObjectiveStep::Attempt, 1001));
        auto const* second = book.ProposeQuest(8, "Earn the second reward", "Another accepted quest", progress);
        ASSERT_NE(second, nullptr);
        nextId = second->id;
        auto const* place = book.ProposePlace(12, "Find useful work", "My private geography");
        ASSERT_NE(place, nullptr);
        placeId = place->id;
    }

    ObjectivePlanningJob Job(uint64_t now = 2000, bool canAsk = true)
    {
        auto job = PrepareObjectivePlanning(owner, 9, book, knowledge, 5, 9, 5, canAsk, now);
        EXPECT_TRUE(job);
        return job.value();
    }

    Bridge::PlanningDecision Choose(ObjectivePlanningJob const& job, std::string capability,
        uint32_t quest, uint32_t place, uint64_t id)
    {
        return Bridge::DecodePlanningDecision({{"version", 1}, {"capability", capability},
            {"quest", quest}, {"place", place}, {"person", nullptr},
            {"evidence", id ? "objective-" + std::to_string(id) : ""},
            {"reason", "Consider this available work after my current commitment."}}, job.issued);
    }

    ActorKey owner{ActorKind::Player, 1};
    ObjectiveBook book;
    PrivateKnowledge knowledge;
    QuestProgress progress;
    uint64_t currentId = 0;
    uint64_t nextId = 0;
    uint64_t placeId = 0;
};
}

TEST_F(AllesObjectivePlanningTest, RepairOptionUsesOwnPreparationEvidenceAndPreservesTheParentQuest)
{
    ASSERT_TRUE(book.Block(currentId, Obstruction::Supplies, "Equipped items need repair", 1500));
    ASSERT_TRUE(book.Defer(currentId, 1500));
    ASSERT_TRUE(book.ProposeRepair(currentId));
    auto job = Job();
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "repair_equipment", 7, 0, currentId), job.issued,
        book, knowledge, 5, false, 2001).status, "equipment_preparation_started");
    ASSERT_NE(book.Preparing(), nullptr);
    EXPECT_EQ(book.Preparing()->id, currentId);
    EXPECT_EQ(book.Find(currentId)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(currentId)->checkpoint, progress);
    EXPECT_FALSE(PrepareObjectivePlanning(owner, 9, book, knowledge, 5, 9, 5, false, 2002));
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "repair_equipment", 7, 0, currentId), job.issued,
        book, knowledge, 5, false, 2003).status, "stale_option");
}

TEST_F(AllesObjectivePlanningTest, IncomePreferenceUsesActualDeficitWithoutInventingMoneyOrCredit)
{
    ASSERT_TRUE(book.Block(currentId, Obstruction::Supplies, "Turn-in needs own money", 1500));
    ASSERT_TRUE(book.Defer(currentId, 1500));
    QuestFinances finances{20, {{7, -100}, {8, 200}, {999, -5000}}, {7, 999}};
    auto const before = book.Capture();
    EXPECT_EQ(MissingQuestMoney(book, finances), 80u);
    EXPECT_EQ(IncomeQuestOrder(book, finances, 5, 2000), std::vector<uint64_t>{nextId});
    EXPECT_EQ(book.Capture(), before);
    EXPECT_EQ(book.Find(currentId)->state, ObjectiveState::Deferred);
    EXPECT_FALSE(book.Find(currentId)->checkpoint.readyToReward);
    finances.ownMoney = 100;
    EXPECT_EQ(MissingQuestMoney(book, finances), 0u);
    EXPECT_TRUE(IncomeQuestOrder(book, finances, 5, 2000).empty());
    EXPECT_EQ(book.Capture(), before); // Only the live readiness sampler may reopen the parent.
    finances.ownMoney = 0;
    finances.turnInCredit.erase(7);
    EXPECT_EQ(MissingQuestMoney(book, finances), 0u);
}

TEST_F(AllesObjectivePlanningTest, IncomeAlternativesRespectObstructionsAndPreferOutstandingRewards)
{
    ASSERT_TRUE(book.Block(currentId, Obstruction::Supplies, "Unpaid turn-in", 1500));
    ASSERT_TRUE(book.Defer(currentId, 1500));
    QuestFinances finances{0, {{7, -100}, {8, 200}, {9, 50}, {10, 80}, {11, -10}}, {7}};
    auto completed = progress;
    completed.readyToReward = true;
    auto const* reward = book.ProposeQuest(9, "Claim reward", "Actual own turn-in", completed);
    ASSERT_NE(reward, nullptr);
    auto const rewardId = reward->id;
    auto const* lower = book.ProposeQuest(10, "Earn less", "Accepted work", progress);
    ASSERT_NE(lower, nullptr);
    auto const lowerId = lower->id;
    ASSERT_NE(book.ProposeQuest(11, "Needs payment", "Accepted work", progress), nullptr);
    auto const expected = std::vector<uint64_t>{rewardId, nextId, lowerId};
    EXPECT_EQ(IncomeQuestOrder(book, finances, 5, 2000), expected);
    ASSERT_TRUE(book.Block(nextId, Obstruction::Strength, "Above current ability", 2000));
    ASSERT_TRUE(book.Defer(nextId, 2000));
    auto const available = std::vector<uint64_t>{rewardId, lowerId};
    EXPECT_EQ(IncomeQuestOrder(book, finances, 5, 2001), available);
    EXPECT_EQ(IncomeQuestOrder(book, finances, 6, 900000), available);
}

TEST_F(AllesObjectivePlanningTest, FinancialContextIsBoundedToSuppliedOwnQuestsAndSignalsActualBalanceChanges)
{
    ASSERT_TRUE(book.Block(currentId, Obstruction::Supplies, "Unpaid turn-in", 1500));
    ASSERT_TRUE(book.Defer(currentId, 1500));
    QuestFinances finances{20, {{7, -100}, {8, 200}, {999, 5000}}, {7}};
    auto const before = book.Capture();
    auto job = PrepareObjectivePlanning(owner, 9, book, knowledge, 5, 9, 5, false, 2000, finances);
    ASSERT_TRUE(job);
    auto const& funds = job->context.at("funds").as_object();
    EXPECT_EQ(funds.at("ownMoney").to_number<uint32_t>(), 20u);
    EXPECT_EQ(funds.at("missingForTurnIns").to_number<uint32_t>(), 80u);
    auto const& amounts = funds.at("acceptedQuestMoney").as_array();
    ASSERT_EQ(amounts.size(), 2u);
    for (auto const& amount : amounts)
        EXPECT_NE(amount.as_object().at("quest").to_number<uint32_t>(), 999u);
    EXPECT_LE(boost::json::serialize(job->context).size(), 7500u);
    EXPECT_EQ(book.Capture(), before);
    auto const signal = ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 2000, finances);
    finances.ownMoney = 21;
    EXPECT_NE(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 2000, finances), signal);
    finances.money.at(7) = std::numeric_limits<int32_t>::min();
    EXPECT_EQ(MissingQuestMoney(book, finances), uint32_t(2147483648ULL - 21));
}

TEST_F(AllesObjectivePlanningTest, SuppliedPurchaseOptionRetainsTheQuestAndRejectsAnUnissuedQuest)
{
    ASSERT_TRUE(book.ProposeSupplies(currentId, 100, 6));
    auto job = Job();
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "buy_quest_supplies", 8, 0, nextId), job.issued,
        book, knowledge, 5, false, 2001).status, "option_not_supplied");
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "buy_quest_supplies", 7, 0, currentId), job.issued,
        book, knowledge, 5, false, 2001).status, "supplies_preparation_started");
    EXPECT_EQ(book.Find(currentId)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(currentId)->checkpoint, progress);
    EXPECT_EQ(book.Preparing()->preparation->item, 100u);
    EXPECT_EQ(book.Preparing()->preparation->count, 6u);
}

TEST_F(AllesObjectivePlanningTest, GroupWorkOffersRecruitmentInsteadOfSoloRetry)
{
    ASSERT_TRUE(book.Block(nextId, Obstruction::Companions, "Needs a willing party", 1500));
    auto job = Job();
    bool recruitment = false;
    for (auto const& option : job.options)
        if (option.quest == 8)
        {
            EXPECT_EQ(option.capability, "seek_companions");
            recruitment = true;
        }
    EXPECT_TRUE(recruitment);
    auto before = book.Capture();
    auto choice = ApplyObjectiveChoice(job, Choose(job, "seek_companions", 8, 0, nextId), job.issued,
        book, knowledge, 5, true, 2010);
    EXPECT_EQ(choice.question, nextId);
    EXPECT_EQ(book.Capture(), before);
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "pursue_quest", 8, 0, nextId), job.issued,
        book, knowledge, 5, true, 2010).status, "option_not_supplied");
}

TEST_F(AllesObjectivePlanningTest, PreferencePersistsWithoutInterruptingOrInventingProgress)
{
    auto job = Job();
    auto before = *book.Current();
    auto choice = ApplyObjectiveChoice(job, Choose(job, "pursue_quest", 8, 0, nextId), job.issued,
        book, knowledge, 5, true, 2001);
    ASSERT_EQ(choice.status, "intention_preferred");
    EXPECT_EQ(choice.selected, nextId);
    EXPECT_EQ(*book.Current(), before);
    EXPECT_EQ(book.Find(nextId)->state, ObjectiveState::Proposed);
    EXPECT_EQ(book.Find(nextId)->checkpoint, progress);
    EXPECT_EQ(book.Find(nextId)->gainedCredit, 0u);
    ASSERT_NE(book.Preferred(2001, 5), nullptr);
    EXPECT_EQ(book.Preferred(2001, 5)->id, nextId);
    PlanningSnapshot snapshot{owner, 1, book.Capture(), knowledge.Capture()};
    auto saved = Storage::DecodePlanning(Storage::EncodePlanning(snapshot), owner);
    ASSERT_TRUE(saved);
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(saved->objectives));
    ASSERT_NE(restored.Preferred(2100, 5), nullptr);
    EXPECT_EQ(restored.Preferred(2100, 5)->id, nextId);
    EXPECT_EQ(restored.Current()->id, currentId);
    EXPECT_EQ(restored.Current()->state, ObjectiveState::Waiting);
}

TEST_F(AllesObjectivePlanningTest, RecentQueuedPreferencePreventsDestinationFlapping)
{
    auto job = Job();
    ASSERT_EQ(ApplyObjectiveChoice(job, Choose(job, "pursue_quest", 8, 0, nextId), job.issued,
        book, knowledge, 5, true, 2001).status, "intention_preferred");
    job = Job(3000);
    auto choice = Choose(job, "discover_work", 0, 12, placeId);
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 3001).status,
        "recent_commitment_held");
    EXPECT_EQ(book.Preferred(3001, 5)->id, nextId);
    job = Job(123000);
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "discover_work", 0, 12, placeId), job.issued,
        book, knowledge, 5, true, 123001).status, "intention_preferred");
    EXPECT_EQ(book.Preferred(123001, 5)->id, placeId);
}

TEST_F(AllesObjectivePlanningTest, SignalsIgnoreTickSamplingButReactToCreditRetryAndEquipmentCircumstances)
{
    auto signal = ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 2000);
    auto job = Job();
    auto revision = book.Current()->revision;
    ASSERT_TRUE(book.Observe(currentId, progress, ObjectiveStep::Attempt, 2001));
    EXPECT_EQ(book.Current()->revision, revision);
    EXPECT_EQ(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 2001), signal);
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "none", 0, 0, 0), job.issued,
        book, knowledge, 5, true, 2002).status, "kept_current_plan");
    EXPECT_NE(ObjectiveDecisionSignal(book, knowledge, 5, 9, 55, 2001), signal);
    ++progress.counters[0];
    ASSERT_TRUE(book.Observe(currentId, progress, ObjectiveStep::Attempt, 3000));
    EXPECT_NE(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 3000), signal);
    ASSERT_TRUE(book.Block(currentId, Obstruction::Navigation, "The current route is obstructed", 4000));
    ASSERT_TRUE(book.Defer(currentId, 4000));
    EXPECT_FALSE(book.Retryable(*book.Find(currentId), 5000, 5));
    EXPECT_FALSE(book.Retryable(*book.Find(currentId), 5000, 55));
    EXPECT_NE(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 5000),
        ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 604000));
    signal = ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 5000);
    book.ReconsiderNavigation(5001);
    EXPECT_TRUE(book.Retryable(*book.Find(currentId), 5001, 5));
    EXPECT_NE(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 5001), signal);
}

TEST_F(AllesObjectivePlanningTest, ReadinessDeferralDoesNotOfferRetriesUntilTheOwnObstructionIsResolved)
{
    ASSERT_TRUE(book.Block(currentId, Obstruction::Strength, "My level is too low for this executor", 4000));
    ASSERT_TRUE(book.Defer(currentId, 4000));
    EXPECT_FALSE(book.Retryable(*book.Find(currentId), 5000, 55));
    EXPECT_EQ(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 5000),
        ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 604000));
    auto const signal = ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 604000);
    ASSERT_TRUE(book.ResolveReadiness(currentId, {}, 604001));
    EXPECT_TRUE(book.Retryable(*book.Find(currentId), 604001, 5));
    EXPECT_NE(ObjectiveDecisionSignal(book, knowledge, 5, 9, 5, 604001), signal);
}

TEST_F(AllesObjectivePlanningTest, AnchorTargetActorAndControlFencesRejectChangedWork)
{
    auto job = Job();
    auto choice = Choose(job, "pursue_quest", 8, 0, nextId);
    auto before = book.Capture();
    auto live = job.issued;
    live.autonomous = false;
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, live, book, knowledge, 5, true, 2001).status, "control_handoff");
    live = job.issued;
    ++live.actorGeneration;
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, live, book, knowledge, 5, true, 2001).status,
        "stale_objective_or_actor");
    EXPECT_EQ(book.Capture(), before);
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 47000).status,
        "stale_decision");
    ASSERT_TRUE(book.Cancel(nextId, "The accepted quest was abandoned"));
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 2001).status, "stale_option");
    ++progress.counters[0];
    ASSERT_TRUE(book.Observe(currentId, progress, ObjectiveStep::Attempt, 2002));
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 2003).status, "stale_decision");
}

TEST_F(AllesObjectivePlanningTest, DecisionCannotInventAnOptionOrUseAnotherObjectivesEvidence)
{
    auto job = Job();
    auto before = book.Capture();
    auto choice = Choose(job, "ask_quest_advice", 8, 0, nextId);
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 2001).status,
        "option_not_supplied");
    choice = Choose(job, "pursue_quest", 8, 0, currentId);
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 2001).status, "invalid_evidence");
    choice = Choose(job, "discover_work", 0, 9999, placeId);
    EXPECT_EQ(ApplyObjectiveChoice(job, choice, job.issued, book, knowledge, 5, true, 2001).status,
        "reference_not_supplied");
    EXPECT_EQ(book.Capture(), before);
}

TEST_F(AllesObjectivePlanningTest, QuestionsAreEligibleAttemptsAndDeferralsRequireMeasuredUnproductiveWork)
{
    for (uint64_t now = 2001; now <= 61001; now += 1000)
        ASSERT_TRUE(book.Observe(currentId, progress, ObjectiveStep::Attempt, now));
    ASSERT_EQ(book.Current()->activeWithoutProgressMs, 60000u);
    auto job = Job(62000);
    auto choice = ApplyObjectiveChoice(job, Choose(job, "defer_quest", 7, 0, currentId), job.issued,
        book, knowledge, 5, true, 62001);
    EXPECT_EQ(choice.status, "deferred");
    EXPECT_EQ(choice.release, currentId);
    EXPECT_EQ(book.Find(currentId)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(currentId)->checkpoint, progress);
    EXPECT_GT(book.Find(currentId)->nextReconsiderationMs, 62001u);
    EXPECT_EQ(book.Current(), nullptr);
    // An information obstruction has a different installed approach from unproductive execution.
    ASSERT_TRUE(book.Block(placeId, Obstruction::Information, "No useful local work", 63000));
    ASSERT_TRUE(book.Defer(placeId, 63000));
    job = Job(64000);
    choice = ApplyObjectiveChoice(job, Choose(job, "ask_place_advice", 0, 12, placeId), job.issued,
        book, knowledge, 5, true, 64001);
    EXPECT_EQ(choice.status, "question_selected");
    EXPECT_EQ(choice.question, placeId);
    EXPECT_EQ(book.Find(placeId)->information.attempts, 0u); // The normal sender must record the actual attempt.
    EXPECT_EQ(ApplyObjectiveChoice(job, Choose(job, "ask_place_advice", 0, 12, placeId), job.issued,
        book, knowledge, 5, false, 64002).status, "question_unavailable");
}

TEST_F(AllesObjectivePlanningTest, ContextBoundsPreservePrivateReportsAndExcludeUnrelatedKnowledge)
{
    ASSERT_TRUE(knowledge.Hear({ActorKey{ActorKind::Player, 2}, "Traveler"}, {Activity::Work, 0, 12, {}},
        "I found no work there yesterday; try carefully.", 1500, 0.4));
    ASSERT_TRUE(knowledge.Hear({ActorKey{ActorKind::Player, 3}, "Other"}, {Activity::Work, 0, 999, {}},
        "Private unrelated location", 1500, 0.4));
    auto job = Job();
    auto text = boost::json::serialize(job.context);
    EXPECT_NE(text.find("no work there yesterday"), std::string::npos);
    EXPECT_NE(text.find("Traveler"), std::string::npos);
    EXPECT_EQ(text.find("Private unrelated location"), std::string::npos);
    EXPECT_EQ(text.find("coordinates"), std::string::npos);
    EXPECT_LE(text.size(), 8192u);
    for (uint32_t quest = 100; quest < 125; ++quest)
        book.ProposeQuest(quest, std::string(500, 'x'), std::string(500, '\n'), progress);
    job = Job();
    EXPECT_LE(boost::json::serialize(job.context).size(), 8192u);
    EXPECT_EQ(job.issued.objective, currentId);
    for (auto const& option : job.options)
        EXPECT_NE(book.Find(option.objective), nullptr);
}
}
