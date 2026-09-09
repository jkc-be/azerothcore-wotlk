/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Objective.h"
#include "gtest/gtest.h"

namespace Alles
{
namespace
{
QuestProgress Accepted(uint32_t credit = 0)
{
    QuestProgress progress;
    progress.inLog = true;
    progress.counters[0] = credit;
    return progress;
}

uint64_t Start(ObjectiveBook& book, QuestProgress progress = Accepted(), uint64_t now = 1000)
{
    auto const* objective = book.ProposeQuest(42, "Earn the reward", "Accepted quest");
    EXPECT_NE(objective, nullptr);
    if (!objective)
        return 0;
    EXPECT_TRUE(book.Activate(objective->id, objective->revision, progress, now, 1));
    return objective->id;
}
}

TEST(AllesObjective, PartialCreditDoesNotMasqueradeAsNewProgress)
{
    ObjectiveBook book({3000, 6000, 3, 32});
    auto progress = Accepted(3);
    auto const id = Start(book, progress);
    for (uint64_t now = 2000; now <= 5000; now += 1000)
        ASSERT_TRUE(book.Observe(id, progress, ObjectiveStep::Attempt, now));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Blocked);
    EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
    EXPECT_EQ(book.Find(id)->obstruction, Obstruction::Executor);
}

TEST(AllesObjective, KillAndItemDeltasResetTheActiveCheckpoint)
{
    ObjectiveBook book({3000, 6000, 3, 32});
    auto progress = Accepted(3);
    auto const id = Start(book, progress);
    book.Observe(id, progress, ObjectiveStep::Attempt, 2000);
    ++progress.counters[0];
    progress.counters[9] = 2;
    book.Observe(id, progress, ObjectiveStep::Attempt, 3000);
    EXPECT_EQ(book.Find(id)->gainedCredit, 3u);
    EXPECT_EQ(book.Find(id)->activeWithoutProgressMs, 0u);
    book.Observe(id, progress, ObjectiveStep::Attempt, 4000);
    EXPECT_EQ(book.Find(id)->gainedCredit, 3u);
    EXPECT_EQ(book.Find(id)->activeWithoutProgressMs, 1000u);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Active);
}

TEST(AllesObjective, TravelRecoveryAndObservationGapsAreNotActiveAttemptTime)
{
    ObjectiveBook book({1000, 6000, 3, 32});
    auto const id = Start(book);
    book.Observe(id, Accepted(), ObjectiveStep::Travel, 2000);
    book.Observe(id, Accepted(), ObjectiveStep::Travel, 100000);
    book.Observe(id, Accepted(), ObjectiveStep::Recover, 101000);
    book.Observe(id, Accepted(), ObjectiveStep::Recover, 102000);
    book.Observe(id, Accepted(), ObjectiveStep::Attempt, 103000);
    book.Observe(id, Accepted(), ObjectiveStep::Attempt, 200000);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Active);
    EXPECT_EQ(book.Find(id)->activeWithoutProgressMs, 0u);
    EXPECT_EQ(book.Find(id)->deaths, 1u);
}

TEST(AllesObjective, CompletionRequiresObservedReward)
{
    ObjectiveBook book;
    auto const id = Start(book);
    auto progress = Accepted(10);
    progress.readyToReward = true;
    book.Observe(id, progress, ObjectiveStep::TurnIn, 2000);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Active);
    progress.inLog = false;
    progress.rewarded = true;
    book.Observe(id, progress, ObjectiveStep::TurnIn, 3000);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Completed);
    EXPECT_EQ(book.Current(), nullptr);
}

TEST(AllesObjective, AbandonmentIsCancellationAndCannotBeRewrittenAsCompletion)
{
    ObjectiveBook book;
    auto const id = Start(book);
    book.Observe(id, {}, ObjectiveStep::Wait, 2000);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Cancelled);
    auto progress = Accepted();
    progress.rewarded = true;
    EXPECT_FALSE(book.Observe(id, progress, ObjectiveStep::Wait, 3000));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Cancelled);
}

TEST(AllesObjective, StaleProposalsAndConcurrentOwnershipAreRejected)
{
    ObjectiveBook book;
    auto const* first = book.ProposeQuest(42, "Reward", "Accepted");
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(book.Activate(first->id, first->revision + 1, Accepted(), 1000, 1));
    ASSERT_TRUE(book.Activate(first->id, first->revision, Accepted(), 1000, 1));
    auto const* second = book.ProposeQuest(43, "Another reward", "Accepted");
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE(book.Activate(second->id, second->revision, Accepted(), 1000, 1));
    EXPECT_EQ(book.Current()->quest, 42u);
}

TEST(AllesObjective, DeferralRetainsQuestAndRetriesOnlyAfterChangeOrWindowWithinAttemptCap)
{
    ObjectiveBook book({3000, 6000, 2, 32});
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Executor, "Executor failed", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    auto const* objective = book.Find(id);
    EXPECT_EQ(objective->quest, 42u);
    EXPECT_TRUE(objective->checkpoint.inLog);
    EXPECT_FALSE(book.Activate(id, objective->revision, Accepted(), 3000, 1));
    EXPECT_TRUE(book.Activate(id, objective->revision, Accepted(), 3000, 2));
    EXPECT_EQ(objective->attempts, 2u);
    book.Block(id, Obstruction::Executor, "Executor failed", 4000);
    book.Defer(id, 4000);
    EXPECT_TRUE(book.Activate(id, objective->revision, Accepted(), 10000, 2));
    book.Block(id, Obstruction::Executor, "Executor failed", 11000);
    book.Defer(id, 11000);
    EXPECT_FALSE(book.Activate(id, objective->revision, Accepted(), 100000, 2));
    EXPECT_TRUE(book.Activate(id, objective->revision, Accepted(), 100000, 3));
    EXPECT_EQ(objective->attemptsInCircumstances, 1u);
}

TEST(AllesObjective, NavigationRetriesBackOffButDoNotPermanentlyExhaustTheQuest)
{
    ObjectiveBook book;
    auto const id = Start(book);
    uint64_t now = 2000;
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        ASSERT_TRUE(book.Block(id, Obstruction::Navigation, "No route", now));
        ASSERT_TRUE(book.Defer(id, now));
        auto const* objective = book.Find(id);
        auto const retry = objective->nextReconsiderationMs;
        EXPECT_GE(retry - now, 60000u);
        EXPECT_LE(retry - now, 600000u);
        EXPECT_FALSE(book.Retryable(*objective, retry - 1, 99)); // Equipment cannot bypass backoff.
        ASSERT_TRUE(book.Activate(id, objective->revision, Accepted(), retry, 1));
        now = retry + 1000;
    }
    EXPECT_EQ(book.Find(id)->attempts, 9u);
}

TEST(AllesObjective, NavigationRecoveryUsesRelocationWithoutReopeningUnrelatedFailures)
{
    ObjectiveBook book;
    auto const id = Start(book);
    book.Block(id, Obstruction::Navigation, "No route", 2000);
    book.Defer(id, 2000);
    auto const* other = book.ProposePlace(363, "Look for work", "No work here");
    ASSERT_TRUE(book.ActivatePlace(other->id, other->revision, 3000, 1));
    book.Block(other->id, Obstruction::Information, "No work here", 4000);
    book.Defer(other->id, 4000);
    auto const otherRevision = other->revision;
    book.ReconsiderNavigation(5000);
    EXPECT_EQ(other->revision, otherRevision);
    EXPECT_FALSE(book.Retryable(*other, 5000, 1));
    EXPECT_TRUE(book.Retryable(*book.Find(id), 5000, 1));
    EXPECT_EQ(book.Find(id)->attempts, 1u);
    EXPECT_EQ(book.Find(id)->attemptsInCircumstances, 0u);
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(book.Capture()));
    EXPECT_TRUE(restored.Retryable(*restored.Find(id), 5000, 1));
}

TEST(AllesObjective, RetryWindowDoesNotMoveOnRepeatedFailedQuestObservations)
{
    ObjectiveBook book({3000, 6000, 3, 32});
    auto const id = Start(book);
    auto progress = Accepted();
    progress.failed = true;
    book.Observe(id, progress, ObjectiveStep::Attempt, 2000);
    book.Defer(id, 2000);
    book.Observe(id, progress, ObjectiveStep::Wait, 3000);
    EXPECT_EQ(book.Find(id)->nextReconsiderationMs, 8000u);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
}

TEST(AllesObjective, HandoffResumesWithoutChargingAnAttemptOrOfflineTime)
{
    ObjectiveBook book;
    auto const id = Start(book);
    book.Observe(id, Accepted(), ObjectiveStep::Attempt, 2000);
    book.Observe(id, Accepted(), ObjectiveStep::Attempt, 3000);
    auto const revision = book.Find(id)->revision;
    ASSERT_TRUE(book.Suspend(id, ObjectiveStep::Wait, "Human control", 4000));
    EXPECT_FALSE(book.Activate(id, revision, Accepted(), 100000, 1));
    ASSERT_TRUE(book.Activate(id, book.Find(id)->revision, Accepted(), 100000, 1));
    EXPECT_EQ(book.Find(id)->attempts, 1u);
    EXPECT_EQ(book.Find(id)->activeWithoutProgressMs, 1000u);
}

TEST(AllesObjective, NewlyEarnedCreditAllowsAnEarlyRetryOfDeferredWork)
{
    ObjectiveBook book({3000, 6000, 1, 32});
    auto const id = Start(book);
    book.Block(id, Obstruction::Executor, "No credit", 2000);
    book.Defer(id, 2000);
    EXPECT_FALSE(book.Activate(id, book.Find(id)->revision, Accepted(), 3000, 1));
    auto progress = Accepted(1);
    progress.readyToReward = true;
    book.Observe(id, progress, ObjectiveStep::Wait, 3000);
    EXPECT_TRUE(book.Activate(id, book.Find(id)->revision, progress, 3000, 1));
}

TEST(AllesObjective, BoundedHistoryEvictsTerminalEntriesButPreservesDeferredIntentions)
{
    ObjectiveBook book({3000, 6000, 3, 2});
    auto const id = Start(book);
    book.Block(id, Obstruction::Information, "Missing location", 2000);
    book.Defer(id, 2000);
    auto const* second = book.ProposeQuest(43, "Reward", "Accepted");
    ASSERT_NE(second, nullptr);
    auto const secondId = second->id;
    EXPECT_EQ(book.ProposeQuest(44, "Reward", "Accepted"), nullptr);
    book.Cancel(secondId, "Request withdrawn");
    EXPECT_NE(book.ProposeQuest(44, "Reward", "Accepted"), nullptr);
    EXPECT_EQ(book.Find(secondId), nullptr);
    EXPECT_NE(book.Find(id), nullptr);
}

TEST(AllesObjective, ReloadRetainsDeferralAndRequiresAuthoritativeRewardReconciliation)
{
    ObjectiveBook book;
    auto const id = Start(book);
    auto progress = Accepted(10);
    progress.rewarded = true;
    progress.inLog = false;
    book.Observe(id, progress, ObjectiveStep::TurnIn, 2000);
    ObjectiveBook reloaded;
    ASSERT_TRUE(reloaded.Restore(book.Capture()));
    EXPECT_EQ(reloaded.Find(id)->state, ObjectiveState::Completed);
    // A crash can leave the authoritative character quest save older than the planning snapshot.
    ASSERT_TRUE(reloaded.Reconcile(id, Accepted(9), 3000));
    EXPECT_EQ(reloaded.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_TRUE(reloaded.Activate(id, reloaded.Find(id)->revision, Accepted(9), 3000, 1));
    EXPECT_EQ(reloaded.Find(id)->checkpoint.counters[0], 9u);
}

TEST(AllesObjective, InvalidRestoreDoesNotDiscardExistingIntention)
{
    ObjectiveBook book;
    auto const id = Start(book);
    auto snapshot = book.Capture();
    snapshot.objectives.at(id).parent = id;
    EXPECT_FALSE(book.Restore(snapshot));
    EXPECT_EQ(book.Current()->state, ObjectiveState::Active);
    snapshot = book.Capture();
    snapshot.objectives.at(id).lastSampleMs = 9999;
    EXPECT_FALSE(book.Restore(snapshot));
    EXPECT_EQ(book.Current()->id, id);
}

TEST(AllesObjective, BackgroundQuestFailureDoesNotCreateASecondExecutionOwner)
{
    ObjectiveBook book;
    auto const active = Start(book);
    auto const* background = book.ProposeQuest(43, "Another reward", "Accepted quest");
    ASSERT_NE(background, nullptr);
    auto observed = Accepted();
    observed.failed = true;
    ASSERT_TRUE(book.Observe(background->id, observed, ObjectiveStep::Wait, 2000));
    ASSERT_NE(book.Current(), nullptr);
    EXPECT_EQ(book.Current()->id, active);
    EXPECT_EQ(background->state, ObjectiveState::Deferred);
    EXPECT_EQ(background->obstruction, Obstruction::Prerequisite);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

TEST(AllesObjective, InvalidTextAndEnumsCannotPoisonAPersistableIntention)
{
    ObjectiveBook book;
    std::string invalidUtf8(1, char(0xc3));
    EXPECT_EQ(book.ProposeQuest(42, invalidUtf8, "Accepted"), nullptr);
    auto const id = Start(book);
    auto const previous = book.Capture();
    EXPECT_FALSE(book.Block(id, Obstruction::Executor, invalidUtf8, 2000));
    EXPECT_FALSE(book.Suspend(id, ObjectiveStep::Wait, invalidUtf8, 2000));
    EXPECT_FALSE(book.Cancel(id, invalidUtf8));
    EXPECT_FALSE(book.Observe(id, Accepted(), ObjectiveStep(255), 2000));
    EXPECT_FALSE(book.Block(id, Obstruction(255), "Invalid obstruction", 2000));
    EXPECT_EQ(book.Capture(), previous);
}

TEST(AllesObjective, HistoryEvictionCannotOrphanASavedChildObjective)
{
    ObjectiveBook book({3000, 6000, 3, 2});
    auto const parent = Start(book);
    ASSERT_TRUE(book.Cancel(parent, "Finished preparing"));
    auto const* child = book.ProposeQuest(43, "Reward", "Prepared work");
    ASSERT_NE(child, nullptr);
    auto saved = book.Capture();
    saved.objectives.at(child->id).parent = parent;
    ASSERT_TRUE(book.Restore(saved));
    EXPECT_EQ(book.ProposeQuest(44, "Reward", "Accepted"), nullptr);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    EXPECT_NE(book.Find(parent), nullptr);
}

TEST(AllesObjective, QuestionsRetainAttemptLimitsAcrossReloadWithoutEstablishingQuestProgress)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Information, "Missing a usable location", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    auto const checkpoint = book.Find(id)->checkpoint;
    ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, "Does anyone have advice?", 3000));
    EXPECT_EQ(book.Find(id)->information.status, InformationStatus::Pending);
    EXPECT_EQ(book.Find(id)->checkpoint, checkpoint);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_FALSE(book.QuestionDelivery(id, 2, true, 3001));
    ASSERT_TRUE(book.QuestionDelivery(id, 1, true, 3001));
    EXPECT_EQ(book.Find(id)->information.status, InformationStatus::Awaiting);
    ObjectiveBook reloaded;
    ASSERT_TRUE(reloaded.Restore(book.Capture()));
    EXPECT_FALSE(reloaded.CanAsk(id, 4000));
    reloaded.ExpireQuestions(123000);
    EXPECT_EQ(reloaded.Find(id)->information.status, InformationStatus::Unanswered);
    EXPECT_FALSE(reloaded.InformationLead(id, 1, 17, 123000));
    ASSERT_TRUE(reloaded.Ask(id, reloaded.Find(id)->revision, "Any suggestions?", 603000));
    EXPECT_FALSE(reloaded.QuestionDelivery(id, 1, true, 603001));
    EXPECT_TRUE(reloaded.QuestionDelivery(id, 2, false, 603001));
    EXPECT_EQ(reloaded.Find(id)->information.status, InformationStatus::Undelivered);
    EXPECT_FALSE(reloaded.CanAsk(id, 9999999));
    EXPECT_TRUE(IsValidObjectiveSnapshot(reloaded.Capture()));
}

TEST(AllesObjective, AUsableReportReopensDeferralButDoesNotCompleteTheObjective)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Information, "Missing a usable location", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    EXPECT_FALSE(book.Ask(id, book.Find(id)->revision + 1, "Where should I look?", 3000));
    ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, "Where should I look?", 3000));
    EXPECT_FALSE(book.InformationLead(id, 1, 17, 3001));
    ASSERT_TRUE(book.QuestionDelivery(id, 1, true, 3001));
    EXPECT_FALSE(book.InformationLead(id, 1, 0, 3002));
    ASSERT_TRUE(book.InformationLead(id, 1, 17, 3002));
    auto const* objective = book.Find(id);
    EXPECT_EQ(objective->information.status, InformationStatus::Lead);
    EXPECT_EQ(objective->information.leadReport, 17u);
    EXPECT_EQ(objective->evidence, std::vector<uint64_t>{17});
    EXPECT_EQ(objective->state, ObjectiveState::Deferred);
    EXPECT_EQ(objective->nextReconsiderationMs, 3002u);
    EXPECT_EQ(objective->gainedCredit, 0u);
    EXPECT_FALSE(objective->checkpoint.rewarded);
    EXPECT_FALSE(book.InformationLead(id, 1, 18, 3003));
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
}

TEST(AllesObjective, QuestionsRequireInformationOrCompanionObstruction)
{
    for (auto const obstruction : {Obstruction::Strength, Obstruction::Navigation})
    {
        ObjectiveBook book;
        auto const id = Start(book);
        book.Block(id, obstruction, "Needs a different approach", 2000);
        book.Defer(id, 2000);
        EXPECT_FALSE(book.CanAsk(id, 3000));
    }
}

TEST(AllesObjective, CompanionObstructionCannotRetrySoloOrConsumeUnlimitedQuestions)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Companions, "Needs willing companions", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    EXPECT_TRUE(book.CanAsk(id, 3000));
    EXPECT_FALSE(book.Retryable(*book.Find(id), 900000, 999));
    ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, "Who would like to join this quest?", 3000));
    ASSERT_TRUE(book.QuestionDelivery(id, 1, false, 3001));
    EXPECT_FALSE(book.CanAsk(id, 602999));
    ASSERT_TRUE(book.Ask(id, book.Find(id)->revision, "Who would like to join this quest?", 603000));
    ASSERT_TRUE(book.QuestionDelivery(id, 2, false, 603001));
    EXPECT_FALSE(book.CanAsk(id, 1203000));
    auto ready = Accepted();
    ready.readyToReward = true;
    ASSERT_TRUE(book.Observe(id, ready, ObjectiveStep::Wait, 1203001));
    EXPECT_FALSE(book.CanAsk(id, 1203002));
    EXPECT_TRUE(book.Retryable(*book.Find(id), 1203002, 999));
    EXPECT_TRUE(book.Activate(id, book.Find(id)->revision, ready, 1203002, 999));
}

TEST(AllesHumanRequest, FollowPreservesQuestProgressAndExpiresWithoutARelogRefund)
{
    ObjectiveBook book;
    auto const quest = Start(book, Accepted(3));
    HumanRequest request{RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Traveler"},
        "Please accompany me to the bridge", "", 2000, 122000};
    auto id = book.Request(request);
    ASSERT_TRUE(id);
    EXPECT_EQ(book.Find(quest)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(quest)->checkpoint, Accepted(3));
    ASSERT_NE(book.Following(), nullptr);
    EXPECT_EQ(book.Following()->request, request);
    EXPECT_EQ(book.Following()->state, ObjectiveState::Proposed);
    EXPECT_FALSE(book.ObserveRequest(*id, {true, false, true, false}, 1999));
    ASSERT_TRUE(book.ObserveRequest(*id, {true, false, false, false}, 3000));
    EXPECT_EQ(book.Find(*id)->state, ObjectiveState::Active);
    EXPECT_EQ(book.Find(*id)->arrivedMs, 0u);
    ASSERT_TRUE(book.ObserveRequest(*id, {true, false, true, false}, 4000));
    EXPECT_EQ(book.Find(*id)->arrivedMs, 4000u);
    ASSERT_TRUE(book.ObserveRequest(*id, {true, true, false, false}, 5000));
    EXPECT_EQ(book.Find(*id)->state, ObjectiveState::Waiting);
    EXPECT_EQ(book.Find(*id)->step, ObjectiveStep::Recover);
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(book.Capture()));
    EXPECT_EQ(restored.Find(*id)->request, request);
    ASSERT_TRUE(restored.ObserveRequest(*id, {true, false, true, false}, 121000));
    ASSERT_TRUE(restored.ObserveRequest(*id, {true, false, true, false}, 122000));
    EXPECT_EQ(restored.Find(*id)->state, ObjectiveState::Completed);
    EXPECT_EQ(restored.Following(), nullptr);
    EXPECT_EQ(restored.Find(quest)->checkpoint, Accepted(3));
    EXPECT_EQ(restored.Find(quest)->gainedCredit, 0u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(restored.Capture()));
}

TEST(AllesHumanRequest, FailedProximityAndUnavailableRequesterCannotMasqueradeAsCompletion)
{
    for (bool available : {false, true})
    {
        ObjectiveBook book;
        auto id = book.Request({RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Traveler"},
            "Follow me", "", 1000, 121000});
        ASSERT_TRUE(id);
        ASSERT_TRUE(book.ObserveRequest(*id, {available, false, false, false}, 121000));
        EXPECT_EQ(book.Find(*id)->state, ObjectiveState::Cancelled);
        EXPECT_EQ(book.Following(), nullptr);
        EXPECT_FALSE(book.Retryable(*book.Find(*id), 1000000, 99));
    }
}

TEST(AllesHumanRequest, EffectsNeedObservationAndDoNotReplaceAnExistingFollow)
{
    ObjectiveBook book;
    auto follow = book.Request({RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Traveler"},
        "Accompany me", "", 1000, 121000});
    ASSERT_TRUE(follow);
    ASSERT_TRUE(book.ObserveRequest(*follow, {true, false, true, false}, 2000));
    for (auto action : {RequestAction::Wave, RequestAction::Assist})
    {
        auto id = book.Request({action, {ActorKey{ActorKind::Player, 9}, "Traveler"},
            "Please help", action == RequestAction::Assist ? "Engaged creature" : "", 3000, 33000});
        ASSERT_TRUE(id);
        EXPECT_EQ(book.Following()->id, *follow);
        ASSERT_TRUE(book.ObserveRequest(*id, {true, false, false, false}, 3001));
        EXPECT_EQ(book.Find(*id)->state, ObjectiveState::Proposed);
        ObjectiveBook interrupted;
        ASSERT_TRUE(interrupted.Restore(book.Capture()));
        EXPECT_EQ(interrupted.Find(*id)->state, ObjectiveState::Cancelled);
        EXPECT_FALSE(interrupted.ObserveRequest(*id, {true, false, false, true}, 3002));
        ASSERT_TRUE(book.ObserveRequest(*id, {true, false, false, true}, 3002));
        EXPECT_EQ(book.Find(*id)->state, ObjectiveState::Completed);
        EXPECT_EQ(book.Following()->id, *follow);
        EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    }
}

TEST(AllesHumanRequest, InvalidRequestsAndConflictingFollowersLeaveTheBookUnchanged)
{
    ObjectiveBook book;
    auto const quest = Start(book);
    auto original = book.Capture();
    for (unsigned invalid = 0; invalid < 7; ++invalid)
    {
        HumanRequest request{RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Traveler"},
            "Follow me", "", 2000, 122000};
        switch (invalid)
        {
            case 0: request.source.actor->kind = ActorKind::CreatureSpawn; break;
            case 1: request.statement.clear(); break;
            case 2: request.source.name.clear(); break;
            case 3: request.expiresMs++; break;
            case 4: request.action = RequestAction(255); break;
            case 5: request.targetName = "Unexpected target"; break;
            case 6: request.source.actor.reset(); break;
        }
        EXPECT_FALSE(book.Request(request)) << invalid;
        EXPECT_EQ(book.Capture(), original) << invalid;
    }
    auto id = book.Request({RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Traveler"},
        "Follow me", "", 2000, 122000});
    ASSERT_TRUE(id);
    original = book.Capture();
    EXPECT_FALSE(book.Request({RequestAction::Follow, {ActorKey{ActorKind::Player, 10}, "Other"},
        "Follow me instead", "", 3000, 123000}));
    EXPECT_EQ(book.Capture(), original);
    EXPECT_EQ(book.Find(quest)->checkpoint, Accepted());
}

TEST(AllesResourcePreparation, RetainsQuestAndExcludesCompetingExecutionUntilEquipmentIsObservedReady)
{
    ObjectiveBook book;
    auto const id = Start(book, Accepted(3));
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Equipped items are critically damaged", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.ProposeRepair(id));
    ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, 3000));
    ASSERT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    auto const* other = book.ProposeQuest(43, "Another quest", "Accepted work", Accepted());
    ASSERT_NE(other, nullptr);
    EXPECT_FALSE(book.Activate(other->id, other->revision, Accepted(), 3001, 1));
    EXPECT_EQ(book.Find(id)->checkpoint, Accepted(3));
    EXPECT_EQ(book.Find(id)->attempts, 1u);
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_FALSE(book.ObserveRepair(id, true, 4000));
    EXPECT_NE(book.Preparing(), nullptr);
    ASSERT_TRUE(book.ObserveRepair(id, false, 5000));
    EXPECT_EQ(book.Find(id)->preparation->state, PreparationState::Completed);
    EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
    EXPECT_FALSE(book.Find(id)->checkpoint.rewarded);
    ASSERT_TRUE(book.ResolveReadiness(id, {}, 5000));
    EXPECT_TRUE(book.Activate(id, book.Find(id)->revision, Accepted(3), 5001, 1));
}

TEST(AllesResourcePreparation, TransactionChargeSurvivesReloadAndDoesNotProveRepairOrRefundRetries)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Repair equipment", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.ProposeRepair(id));
    for (uint64_t attempt = 0; attempt < 3; ++attempt)
    {
        auto const now = 3000 + attempt * 601000;
        ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, now));
        ASSERT_TRUE(book.PreparationTransaction(id, now + 1));
        EXPECT_FALSE(book.PreparationTransaction(id, now + 2));
        ASSERT_TRUE(book.PreparationExpense(id, 100, 90));
        EXPECT_FALSE(book.PreparationExpense(id, 90, 100));
        ObjectiveBook restored;
        ASSERT_TRUE(restored.Restore(book.Capture()));
        book = std::move(restored);
        EXPECT_FALSE(book.PreparationTransaction(id, now + 3));
        EXPECT_EQ(book.Find(id)->preparation->state, PreparationState::Active);
        EXPECT_FALSE(book.ObserveRepair(id, true, now + 3));
        ASSERT_TRUE(book.DeferPreparation(id, "Not enough money to finish the repairs", now + 4));
        EXPECT_FALSE(book.CanRepair(id, now + 5));
        EXPECT_FALSE(book.ProposeRepair(id));
    }
    EXPECT_FALSE(book.CanRepair(id, 9000000));
    EXPECT_EQ(book.Find(id)->preparation->attempts, 3u);
    EXPECT_EQ(book.Find(id)->preparation->transactions, 3u);
    EXPECT_EQ(book.Find(id)->preparation->spentMoney, 30u);
    EXPECT_EQ(book.Find(id)->attempts, 1u);
}

TEST(AllesResourcePreparation, MissingRepairerAndRecoveryCannotRenewTheSavedDeadline)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Repair equipment", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.ProposeRepair(id));
    ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, 3000));
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(book.Capture()));
    EXPECT_FALSE(restored.ObserveRepair(id, true, 122999));
    EXPECT_TRUE(restored.ObserveRepair(id, true, 123000));
    EXPECT_EQ(restored.Find(id)->preparation->state, PreparationState::Deferred);
    EXPECT_EQ(restored.Find(id)->preparation->transactions, 0u);
    EXPECT_FALSE(restored.PreparationTransaction(id, 123001));
    EXPECT_EQ(restored.Find(id)->checkpoint, Accepted());
}

TEST(AllesResourcePreparation, HumanFollowAndParentCancellationEndPreparationWithoutDroppingTheQuest)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Repair equipment", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.ProposeRepair(id));
    ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, 3000));
    auto requested = book.Request({RequestAction::Follow, {ActorKey{ActorKind::Player, 9}, "Human"},
        "Follow me", "", 4000, 124000});
    ASSERT_TRUE(requested);
    EXPECT_EQ(book.Preparing(), nullptr);
    EXPECT_EQ(book.Find(id)->preparation->state, PreparationState::Deferred);
    EXPECT_EQ(book.Find(id)->checkpoint, Accepted());
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    ASSERT_TRUE(book.Cancel(*requested, "Stopped"));
    ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, 605000));
    ASSERT_TRUE(book.Cancel(id, "The quest was abandoned"));
    EXPECT_EQ(book.Preparing(), nullptr);
    EXPECT_EQ(book.Find(id)->preparation->state, PreparationState::Cancelled);
}

TEST(AllesSupplyPreparation, ReceivedOfferCanChangeApproachButOnlyOwnInventoryResolvesTheNeed)
{
    ObjectiveBook book;
    auto const id = Start(book, Accepted(3));
    ASSERT_TRUE(book.ProposeSupplies(id, 100, 6));
    EXPECT_FALSE(book.CanRepair(id, 2000));
    ASSERT_TRUE(book.BeginSupplyPurchase(id, book.Find(id)->revision, 2000));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(id)->checkpoint, Accepted(3));
    EXPECT_EQ(book.Find(id)->attempts, 1u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    EXPECT_FALSE(book.ObserveRepair(id, false, 2001));
    ASSERT_TRUE(book.PreparationTransaction(id, 2001));
    EXPECT_FALSE(book.ObserveSupplies(id, 5, 2002));
    EXPECT_EQ(book.Find(id)->preparation->state, PreparationState::Active);
    EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
    ASSERT_TRUE(book.ObserveSupplies(id, 6, 2003));
    EXPECT_EQ(book.Find(id)->preparation->state, PreparationState::Completed);
    EXPECT_FALSE(book.Find(id)->checkpoint.rewarded);
    EXPECT_EQ(book.Find(id)->checkpoint, Accepted(3)); // The separate authoritative quest sample establishes credit.
    ASSERT_TRUE(book.Activate(id, book.Find(id)->revision, Accepted(3), 2004, 1));
}

TEST(AllesResourcePreparation, MoreObservedFundsReopenExhaustedAttemptsButReloadAndOwnSpendingDoNot)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Need repair", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    ASSERT_TRUE(book.ProposeRepair(id));
    for (uint64_t attempt = 0; attempt < 3; ++attempt)
    {
        auto const now = 3000 + attempt * 601000;
        ASSERT_TRUE(book.BeginRepair(id, book.Find(id)->revision, now));
        ASSERT_TRUE(book.DeferPreparation(id, "Need more own funds", now + 1));
    }
    EXPECT_FALSE(book.CanRepair(id, 9000000));
    // A legacy snapshot had no saved balance: the first observation cannot assert that money increased.
    ASSERT_TRUE(book.ObservePreparationFunds(id, 50, 9000000));
    EXPECT_FALSE(book.CanRepair(id, 9000000));
    auto snapshot = book.Capture();
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(snapshot));
    EXPECT_FALSE(restored.ObservePreparationFunds(id, 50, 9000001));
    ASSERT_TRUE(restored.ObservePreparationFunds(id, 40, 9000002));
    EXPECT_FALSE(restored.CanRepair(id, 9000002));
    ASSERT_TRUE(restored.ObservePreparationFunds(id, 51, 9000003));
    EXPECT_TRUE(restored.CanRepair(id, 9000003));
    EXPECT_EQ(restored.Find(id)->preparation->attempts, 3u);
    EXPECT_EQ(restored.Find(id)->preparation->attemptsInCircumstances, 0u);
    ASSERT_TRUE(restored.BeginRepair(id, restored.Find(id)->revision, 9000004));
    EXPECT_EQ(restored.Find(id)->preparation->attempts, 4u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(restored.Capture()));
}

TEST(AllesQuestReadiness, TurnInNeedsAndExecutorSupportAreNotConfusedWithCompanionNeeds)
{
    QuestReadiness readiness;
    readiness.aboveLevelCapability = true;
    readiness.needsCompanions = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::Strength);
    readiness.criticallyDamagedEquipment = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::Supplies);
    readiness.unsupportedExecutor = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::Executor);
    readiness.turningIn = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::None);
    readiness.insufficientMoney = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::Supplies);
    readiness.failed = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::Prerequisite);
    readiness.rewarded = true;
    EXPECT_EQ(QuestReadinessObstruction(readiness), Obstruction::None);
}

TEST(AllesQuestReadiness, UnrelatedChangesCannotReopenUnresolvedStrengthSuppliesOrPrerequisites)
{
    for (auto reason : {Obstruction::Strength, Obstruction::Supplies, Obstruction::Prerequisite})
    {
        ObjectiveBook book;
        auto const id = Start(book, Accepted(3));
        ASSERT_TRUE(book.Block(id, reason, "Own readiness condition", 2000));
        ASSERT_TRUE(book.Defer(id, 2000));
        EXPECT_FALSE(book.Retryable(*book.Find(id), 9000000, 999));
        QuestReadiness stillBlocked;
        stillBlocked.failed = reason == Obstruction::Prerequisite;
        stillBlocked.aboveLevelCapability = reason == Obstruction::Strength;
        stillBlocked.criticallyDamagedEquipment = reason == Obstruction::Supplies;
        auto const before = book.Capture();
        EXPECT_FALSE(book.ResolveReadiness(id, stillBlocked, 3000));
        EXPECT_EQ(book.Capture(), before);
        ASSERT_TRUE(book.ResolveReadiness(id, {}, 4000));
        EXPECT_TRUE(book.Retryable(*book.Find(id), 4000, 1));
        EXPECT_EQ(book.Find(id)->checkpoint, Accepted(3));
        EXPECT_EQ(book.Find(id)->attempts, 1u);
        EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
        EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
        EXPECT_FALSE(book.ResolveReadiness(id, {}, 5000));
    }
}

TEST(AllesQuestReadiness, AnotherUnresolvedConditionUpdatesTheDiagnosisWithoutRefundingTheAttempt)
{
    ObjectiveBook book;
    auto const id = Start(book, Accepted(3));
    ASSERT_TRUE(book.Block(id, Obstruction::Supplies, "Equipped items need repair", 2000));
    ASSERT_TRUE(book.Defer(id, 2000));
    auto const before = *book.Find(id);
    // After repair the own level is still below the supported range. Another repair cannot fix this condition.
    ASSERT_TRUE(book.Block(id, Obstruction::Strength, "My level is below the supported range", 3000));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_EQ(book.Find(id)->obstruction, Obstruction::Strength);
    EXPECT_EQ(book.Find(id)->nextReconsiderationMs, before.nextReconsiderationMs);
    EXPECT_EQ(book.Find(id)->attempts, before.attempts);
    EXPECT_EQ(book.Find(id)->checkpoint, before.checkpoint);
    EXPECT_FALSE(book.Retryable(*book.Find(id), 9000000, 999));
    EXPECT_FALSE(book.Block(id, Obstruction::Executor, "An unrelated timeout", 4000));
    ASSERT_TRUE(book.ResolveReadiness(id, {}, 5000));
    EXPECT_TRUE(book.Retryable(*book.Find(id), 5000, 1));
}

TEST(AllesQuestOpportunity, AnEmptyOrTruncatedScanDoesNotInventCompetitionOrARespawn)
{
    EXPECT_EQ(ClassifyQuestOpportunity(0, 0, 0, true), QuestOpportunity::Unknown);
    EXPECT_EQ(ClassifyQuestOpportunity(0, 2, 0, false), QuestOpportunity::Unknown);
    EXPECT_EQ(ClassifyQuestOpportunity(0, 0, 2, false), QuestOpportunity::Unknown);
    EXPECT_EQ(ClassifyQuestOpportunity(1, 2, 3, false), QuestOpportunity::Available);
    EXPECT_EQ(ClassifyQuestOpportunity(0, 2, 3, true), QuestOpportunity::Tagged);
    EXPECT_EQ(ClassifyQuestOpportunity(0, 0, 3, true), QuestOpportunity::Respawn);
}

TEST(AllesQuestOpportunity, WaitingDoesNotCountAsActiveWorkAndAvailabilityResumesWithoutAnotherAttempt)
{
    ObjectiveBook book;
    auto const id = Start(book, Accepted(3));
    ASSERT_TRUE(book.Observe(id, Accepted(3), ObjectiveStep::Attempt, 2000));
    ASSERT_TRUE(book.Observe(id, Accepted(3), ObjectiveStep::Attempt, 3000));
    auto const attempted = book.Find(id)->activeWithoutProgressMs;
    ASSERT_TRUE(book.ObserveOpportunity(id, QuestOpportunity::Tagged, 4000));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Waiting);
    EXPECT_EQ(book.Find(id)->obstruction, Obstruction::Competition);
    auto const deadline = book.Find(id)->nextReconsiderationMs;
    for (uint64_t now = 5000; now < 9000; now += 1000)
    {
        EXPECT_FALSE(book.ObserveOpportunity(id, QuestOpportunity::Tagged, now));
        ASSERT_TRUE(book.Observe(id, Accepted(3), ObjectiveStep::Wait, now));
    }
    EXPECT_EQ(book.Find(id)->activeWithoutProgressMs, attempted);
    EXPECT_EQ(book.Find(id)->nextReconsiderationMs, deadline);
    ASSERT_TRUE(book.ObserveOpportunity(id, QuestOpportunity::Available, 9000));
    EXPECT_EQ(book.Find(id)->state, ObjectiveState::Active);
    EXPECT_EQ(book.Find(id)->obstruction, Obstruction::None);
    EXPECT_EQ(book.Find(id)->attempts, 1u);
    EXPECT_EQ(book.Find(id)->checkpoint, Accepted(3));
    EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
}

TEST(AllesQuestOpportunity, ReloadPreservesTheWaitDeadlineAndMissingFreshSightDoesNotRefundIt)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.ObserveOpportunity(id, QuestOpportunity::Respawn, 2000));
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(book.Capture()));
    EXPECT_EQ(restored.Find(id)->nextReconsiderationMs, 62000u);
    EXPECT_FALSE(restored.ObserveOpportunity(id, QuestOpportunity::Unknown, 61000));
    ASSERT_TRUE(restored.ObserveOpportunity(id, QuestOpportunity::Unknown, 62000));
    EXPECT_EQ(restored.Find(id)->state, ObjectiveState::Blocked);
    EXPECT_EQ(restored.Find(id)->obstruction, Obstruction::Competition);
    ASSERT_TRUE(restored.Defer(id, 62000));
    EXPECT_EQ(restored.Find(id)->state, ObjectiveState::Deferred);
    EXPECT_EQ(restored.Find(id)->checkpoint, Accepted());
}

TEST(AllesQuestOpportunity, SpecificNavigationEvidenceRefinesGenericTimeoutWithoutRefundingAttempts)
{
    ObjectiveBook book;
    auto const id = Start(book);
    ASSERT_TRUE(book.Block(id, Obstruction::Executor, "No active progress", 2000));
    auto const deadline = book.Find(id)->nextReconsiderationMs;
    ASSERT_TRUE(book.Block(id, Obstruction::Navigation, "Invalid route height", 3000));
    EXPECT_EQ(book.Find(id)->obstruction, Obstruction::Navigation);
    EXPECT_EQ(book.Find(id)->nextReconsiderationMs, deadline);
    EXPECT_EQ(book.Find(id)->attempts, 1u);
    EXPECT_FALSE(book.Block(id, Obstruction::Companions, "Recruit someone instead", 4000));
    EXPECT_EQ(book.Find(id)->obstruction, Obstruction::Navigation);
}

TEST(AllesResourcePreparation, ActualSaleIncomeAndPurchaseExpenseRemainDistinctAcrossReload)
{
    ObjectiveBook book;
    auto const id = Start(book, Accepted(3));
    ASSERT_TRUE(book.ProposeSupplies(id, 100, 6));
    ASSERT_TRUE(book.ObservePreparationFunds(id, 10, 2000));
    ASSERT_TRUE(book.BeginSupplyPurchase(id, book.Find(id)->revision, 2001));
    EXPECT_FALSE(book.PreparationIncome(id, 10, 30));
    ASSERT_TRUE(book.PreparationTransaction(id, 2002));
    EXPECT_FALSE(book.PreparationIncome(id, 10, 10));
    EXPECT_FALSE(book.PreparationIncome(id, 10, 9));
    ASSERT_TRUE(book.PreparationIncome(id, 10, 30));
    ASSERT_TRUE(book.PreparationExpense(id, 30, 5));
    auto const* preparation = &*book.Find(id)->preparation;
    EXPECT_EQ(preparation->earnedMoney, 20u);
    EXPECT_EQ(preparation->spentMoney, 25u);
    EXPECT_EQ(preparation->ownMoney, 5u);
    EXPECT_EQ(preparation->fundsAtAttempt, 10u);
    EXPECT_EQ(book.Find(id)->checkpoint, Accepted(3));
    EXPECT_EQ(book.Find(id)->gainedCredit, 0u);
    EXPECT_TRUE(IsValidObjectiveSnapshot(book.Capture()));
    ObjectiveBook restored;
    ASSERT_TRUE(restored.Restore(book.Capture()));
    EXPECT_FALSE(restored.PreparationTransaction(id, 3000));
    ASSERT_TRUE(restored.DeferPreparation(id, "Real sale did not fund the required purchase", 3000));
    EXPECT_FALSE(restored.PreparationIncome(id, 5, 30));
    EXPECT_FALSE(restored.ObservePreparationFunds(id, 5, 3001));
    EXPECT_FALSE(restored.CanBuySupplies(id, 3001));
    EXPECT_EQ(restored.Find(id)->preparation->earnedMoney, 20u);
}

}
