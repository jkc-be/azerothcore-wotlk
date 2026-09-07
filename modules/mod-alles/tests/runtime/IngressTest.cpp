/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "runtime/Ingress.h"
#include "gtest/gtest.h"
#include <atomic>
#include <thread>

namespace Alles
{
TEST(AllesIngress, SpeechCannotFillTheLifecycleReserve)
{
    Ingress ingress(4, 1);
    IngressEvent perception{IngressKind::Observe, {ActorKind::Player, 1}, 100, {}};
    for (unsigned index = 0; index < 3; ++index)
        ASSERT_TRUE(ingress.Push(perception));
    EXPECT_FALSE(ingress.Push(perception));
    ASSERT_TRUE(ingress.Push({IngressKind::Logout, perception.owner, 100, {}}));
    EXPECT_FALSE(ingress.Push({IngressKind::Activate, perception.owner, 101, {}}));
    auto const first = ingress.Drain(2);
    ASSERT_EQ(first.size(), 2u);
    auto const second = ingress.Drain(4);
    ASSERT_EQ(second.size(), 2u);
    EXPECT_EQ(second.back().kind, IngressKind::Logout);
    EXPECT_EQ(second.back().attachment, 100u);
    EXPECT_TRUE(ingress.Drain(4).empty());
}

TEST(AllesIngress, ForeignPlaintextIsDestroyedBeforeRetention)
{
    Ingress ingress;
    IngressEvent event{IngressKind::Observe, {ActorKind::Player, 1}, 100, {}};
    event.perception.comprehended = false;
    event.perception.text = "an inaccessible secret";
    ASSERT_TRUE(ingress.Push(event));
    auto const events = ingress.Drain(1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events.front().perception.text.empty());
    EXPECT_FALSE(events.front().perception.comprehended);
}

TEST(AllesIngress, ConcurrentProducersKeepEachAttachmentSequenceOrdered)
{
    Ingress ingress(256, 64);
    std::atomic<unsigned> rejected{0};
    auto produce = [&](uint64_t id)
    {
        for (uint64_t sequence = 1; sequence <= 64; ++sequence)
            if (!ingress.Push({IngressKind::Save, {ActorKind::Player, id}, sequence, {}}))
                ++rejected;
    };
    std::thread first(produce, 1);
    std::thread second(produce, 2);
    first.join();
    second.join();
    EXPECT_EQ(rejected, 0u);
    auto const events = ingress.Drain(256);
    ASSERT_EQ(events.size(), 128u);
    uint64_t previous[2] = {0, 0};
    for (auto const& event : events)
    {
        auto& last = previous[event.owner.id - 1];
        EXPECT_EQ(event.attachment, last + 1);
        last = event.attachment;
    }
    EXPECT_EQ(previous[0], 64u);
    EXPECT_EQ(previous[1], 64u);
}

TEST(AllesIngress, OldLogoutAndNewActivationKeepDistinctTokensInTheQueue)
{
    Ingress ingress;
    ActorKey const owner{ActorKind::Player, 42};
    ASSERT_TRUE(ingress.Push({IngressKind::Activate, owner, 11, {}}));
    ASSERT_TRUE(ingress.Push({IngressKind::Logout, owner, 10, {}}));
    auto const events = ingress.Drain(2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].attachment, 11u);
    EXPECT_EQ(events[1].attachment, 10u);
    EXPECT_EQ(events[1].kind, IngressKind::Logout);
}
}
