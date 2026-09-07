/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "runtime/CommandsParse.h"
#include "gtest/gtest.h"
#include <limits>

namespace
{
using namespace Alles;

TEST(AllesCommandsParseTest, RecallHasBoundedCountsAndCannotSelectAnotherOwner)
{
    for (auto const* input : {"", "  \t "})
    {
        auto const args = Commands::ParseArguments(input);
        ASSERT_TRUE(args.has_value());
        EXPECT_EQ(Commands::RecallCount(*args), 5u);
    }
    for (auto const* input : {"1", "20", "  10\t"})
    {
        auto const args = Commands::ParseArguments(input);
        ASSERT_TRUE(args.has_value());
        EXPECT_TRUE(Commands::RecallCount(*args).has_value());
    }
    for (auto const* input : {"0", "21", "-1", "+1", "1x", "1 2", "player 17", "creature 17", "Humanb"})
    {
        auto const args = Commands::ParseArguments(input);
        ASSERT_TRUE(args.has_value());
        EXPECT_FALSE(Commands::RecallCount(*args).has_value());
    }
}

TEST(AllesCommandsParseTest, TypedOwnerSupportsExact64BitIdsAndEqualIdIsolation)
{
    auto const playerArgs = Commands::ParseArguments("player 18446744073709551615");
    auto const creatureArgs = Commands::ParseArguments("creature 18446744073709551615");
    ASSERT_TRUE(playerArgs.has_value());
    ASSERT_TRUE(creatureArgs.has_value());
    auto const player = Commands::Owner(*playerArgs);
    auto const creature = Commands::Owner(*creatureArgs);
    ASSERT_TRUE(player.has_value());
    ASSERT_TRUE(creature.has_value());
    EXPECT_EQ(player->id, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(creature->id, player->id);
    EXPECT_EQ(player->kind, ActorKind::Player);
    EXPECT_EQ(creature->kind, ActorKind::CreatureSpawn);
    EXPECT_NE(*player, *creature);
}

TEST(AllesCommandsParseTest, OwnerRejectsMissingAmbiguousAndTrailingInputs)
{
    char const* const invalid[] =
    {
        "", "player", "17", "Player 17", "npc 17", "player 0", "player -1", "player +1",
        "player 0x10", "player 1x", "player 1.0", "player 18446744073709551616", "player 1 extra",
        "player\n17", "player 1\n", "player |Hplayer:Humanb|h[Humanb]|h"
    };
    for (auto const* input : invalid)
    {
        SCOPED_TRACE(input);
        auto const args = Commands::ParseArguments(input);
        EXPECT_FALSE(args && Commands::Owner(*args));
    }
}

TEST(AllesCommandsParseTest, ParserBoundsInputBeforeTokenization)
{
    EXPECT_TRUE(Commands::ParseArguments(nullptr).has_value());
    EXPECT_TRUE(Commands::ParseArguments(std::string(128, ' ').c_str()).has_value());
    EXPECT_FALSE(Commands::ParseArguments(std::string(129, ' ').c_str()).has_value());
    EXPECT_FALSE(Commands::ParseArguments("player 1 extra").has_value());
}
}
