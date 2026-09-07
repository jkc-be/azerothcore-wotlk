/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "runtime/RuntimeSettings.h"
#include "gtest/gtest.h"

namespace Alles
{
TEST(AllesRuntimeSettings, EqualNumericIdsRequireAndPreserveTheirKinds)
{
    auto const owners = ParseOwners("player:42, creature:42", 2);
    ASSERT_TRUE(owners);
    EXPECT_EQ(owners->size(), 2u);
    EXPECT_TRUE(owners->contains({ActorKind::Player, 42}));
    EXPECT_TRUE(owners->contains({ActorKind::CreatureSpawn, 42}));
    EXPECT_FALSE(ParseOwners("42,43", 2));
}

TEST(AllesRuntimeSettings, ExactUint64AndEntryLimitsAreEnforced)
{
    EXPECT_TRUE(ParseOwners("creature:18446744073709551615", 1));
    EXPECT_FALSE(ParseOwners("creature:18446744073709551616", 1));
    EXPECT_FALSE(ParseOwners("player:1,player:2", 1));
    EXPECT_FALSE(ParseOwners("player:1", 0));
    EXPECT_FALSE(ParseOwners("player:1,player:1", 2));
}

TEST(AllesRuntimeSettings, MalformedListsCannotWidenTheAllowlist)
{
    for (auto const* text : {"", "player:0", "player:-1", "player:+1", "player:1x", "PLAYER:1",
        "player:1,", ",player:1", "player:1,,player:2", "player: 1", "player:1;creature:2", "player:1\n"})
        EXPECT_FALSE(ParseOwners(text, 64)) << text;
}
}
