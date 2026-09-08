/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "interpreter/Scheduler.h"
#include "gtest/gtest.h"

namespace Alles::Interpreter
{
namespace
{
std::vector<JobPurpose> const All{JobPurpose::Memory, JobPurpose::Planning, JobPurpose::Conversation,
    JobPurpose::Interview};
ScheduledJob Job(std::string id, uint64_t actor, JobPurpose purpose = JobPurpose::Memory, uint64_t admitted = 0)
{
    return {std::move(id), {ActorKind::Player, actor}, 1, purpose, {}, {}, admitted, admitted + 20000, 1024};
}
}

TEST(AllesScheduler, NoisyActorCannotStarveAnotherActor)
{
    Scheduler scheduler;
    ASSERT_TRUE(scheduler.Admit(Job("noisy-1", 1, JobPurpose::Conversation)).empty());
    ASSERT_TRUE(scheduler.Admit(Job("noisy-2", 1, JobPurpose::Conversation)).empty());
    ASSERT_TRUE(scheduler.Admit(Job("quiet", 2, JobPurpose::Interview)).empty());
    ASSERT_EQ(scheduler.Select(1, All)->id, "noisy-1");
    ASSERT_TRUE(scheduler.Reserve("noisy-1", 1));
    EXPECT_FALSE(scheduler.Select(1, All));
    scheduler.Finish("noisy-1", "completed");
    ASSERT_TRUE(scheduler.Admit(Job("noisy-3", 1, JobPurpose::Conversation)).empty());
    EXPECT_EQ(scheduler.Select(2, All)->id, "quiet");
}

TEST(AllesScheduler, ConcurrentActorsAndCausalOrdering)
{
    SchedulingPolicy policy;
    policy.concurrentJobs = 2;
    Scheduler scheduler(policy);
    auto first = Job("first", 1, JobPurpose::Interview);
    first.causalKey = "thread";
    auto second = Job("second", 1, JobPurpose::Conversation);
    second.causalKey = "thread";
    ASSERT_TRUE(scheduler.Admit(first).empty());
    ASSERT_TRUE(scheduler.Admit(second).empty());
    ASSERT_TRUE(scheduler.Admit(Job("other", 2)).empty());
    EXPECT_EQ(scheduler.Select(1, All)->id, "first");
    ASSERT_TRUE(scheduler.Reserve("first", 1));
    EXPECT_FALSE(scheduler.Reserve("second", 1));
    ASSERT_TRUE(scheduler.Reserve("other", 1));
    EXPECT_EQ(scheduler.Active(), 2u);
    policy.concurrentJobs = 1;
    scheduler.Apply(policy);
    EXPECT_EQ(scheduler.Active(), 2u); // Shrinking drains; it does not cancel physical execution.
    scheduler.Finish("first", "cancel_acknowledged");
    EXPECT_FALSE(scheduler.Select(2, All));
    scheduler.Finish("other", "completed");
    EXPECT_EQ(scheduler.Select(2, All)->id, "second");
}

TEST(AllesScheduler, ReplacementPreservesAgeAndCannotCrossDecisionOrGeneration)
{
    Scheduler scheduler;
    auto old = Job("old", 1, JobPurpose::Planning);
    old.replacementKey = "objective-7";
    ASSERT_TRUE(scheduler.Admit(old).empty());
    auto next = Job("new", 1, JobPurpose::Planning, 19000);
    next.replacementKey = old.replacementKey;
    ASSERT_TRUE(scheduler.Admit(next).empty());
    auto selected = scheduler.Select(19001, All);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->admittedMs, 0u);
    EXPECT_EQ(selected->expiresMs, 20000u);
    next.id = "other-generation";
    next.generation = 2;
    ASSERT_TRUE(scheduler.Admit(next).empty());
    scheduler.Expire(20000);
    EXPECT_FALSE(scheduler.Contains("new"));
    EXPECT_TRUE(scheduler.Contains("other-generation"));
    EXPECT_FALSE(scheduler.Reserve("new", 20001)); // Old/late descriptors cannot revive.
}

TEST(AllesScheduler, BytesCountsAndShrinkHaveExplicitOutcomes)
{
    SchedulingPolicy policy;
    policy.waitingPerActor = 3;
    policy.bytesPerActor = 2048;
    Scheduler scheduler(policy);
    ASSERT_TRUE(scheduler.Admit(Job("a", 1)).empty());
    ASSERT_TRUE(scheduler.Admit(Job("b", 1)).empty());
    EXPECT_EQ(scheduler.Admit(Job("too-large", 1)), "queue_capacity");
    ASSERT_TRUE(scheduler.Admit(Job("interview", 2, JobPurpose::Interview)).empty());
    policy.waitingPerActor = policy.waitingGlobal = 1;
    scheduler.Apply(policy);
    EXPECT_TRUE(scheduler.Contains("a"));
    EXPECT_FALSE(scheduler.Contains("b"));
    EXPECT_FALSE(scheduler.Contains("interview"));
    auto outcomes = scheduler.TakeOutcomes();
    ASSERT_EQ(outcomes.size(), 2u);
    EXPECT_EQ(outcomes[0].reason, "policy_capacity");
}

TEST(AllesScheduler, AgingPromotesOldIndependentReadOnlyWork)
{
    Scheduler scheduler;
    ASSERT_TRUE(scheduler.Admit(Job("old", 1, JobPurpose::Interview)).empty());
    ASSERT_TRUE(scheduler.Admit(Job("new", 1, JobPurpose::Conversation, 16000)).empty());
    EXPECT_EQ(scheduler.Select(16001, All)->id, "old");
    EXPECT_THROW(Scheduler(SchedulingPolicy{0}), std::invalid_argument);
}
}
