/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "storage/SnapshotDao.h"
#include "storage/SnapshotStatements.h"
#include "gtest/gtest.h"

namespace Alles::Storage
{
namespace
{
constexpr uint64 LargeId = (uint64(1) << 40) + 37;

TEST(AllesSnapshotStatementsTest, EqualPlayerAndSpawnIdsRemainDistinctAtTheDatabaseBoundary)
{
    CharacterDatabasePreparedStatement player(CHAR_DEL_ALLES_MEMORIES, 2);
    CharacterDatabasePreparedStatement creature(CHAR_DEL_ALLES_MEMORIES, 2);
    BindOwner(player, {ActorKind::Player, LargeId});
    BindOwner(creature, {ActorKind::CreatureSpawn, LargeId});
    EXPECT_EQ(std::get<uint8>(player.GetParameters()[0].data), uint8(ActorKind::Player));
    EXPECT_EQ(std::get<uint8>(creature.GetParameters()[0].data), uint8(ActorKind::CreatureSpawn));
    EXPECT_EQ(std::get<uint64>(player.GetParameters()[1].data), LargeId);
    EXPECT_EQ(std::get<uint64>(creature.GetParameters()[1].data), LargeId);
}

TEST(AllesSnapshotStatementsTest, CountersRevisionAndGameTimeRemain64Bit)
{
    OwnerSnapshot snapshot;
    snapshot.owner = {ActorKind::Player, LargeId};
    snapshot.revision = LargeId + 1;
    snapshot.nextPerceptionId = LargeId + 2;
    snapshot.nextMemoryId = LargeId + 3;
    snapshot.decayGameTimeMs = LargeId + 4;
    snapshot.nextConsolidationGameTimeMs = LargeId + 5;
    CharacterDatabasePreparedStatement statement(CHAR_REP_ALLES_ACTOR, 7);
    BindActor(statement, snapshot);
    auto const& parameters = statement.GetParameters();
    ASSERT_EQ(parameters.size(), 7u);
    for (std::size_t index = 1; index < parameters.size(); ++index)
        EXPECT_EQ(std::get<uint64>(parameters[index].data), LargeId + index - 1);
}

TEST(AllesSnapshotStatementsTest, UnknownIdentityAndDepthRemainSqlNull)
{
    Memory memory;
    memory.subject.name = "a passing summon";
    memory.source = {ActorKey{ActorKind::CreatureSpawn, LargeId}, "Innkeeper"};
    memory.confidence = 0.6;
    memory.salience = 0.2;
    CharacterDatabasePreparedStatement statement(CHAR_INS_ALLES_MEMORY, 23);
    BindMemory(statement, {ActorKind::Player, 1}, memory);
    auto const& parameters = statement.GetParameters();
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(parameters[5].data));
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(parameters[6].data));
    EXPECT_EQ(std::get<std::string>(parameters[7].data), "a passing summon");
    EXPECT_EQ(std::get<uint8>(parameters[8].data), uint8(ActorKind::CreatureSpawn));
    EXPECT_EQ(std::get<uint64>(parameters[9].data), LargeId);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(parameters[13].data));
    EXPECT_DOUBLE_EQ(std::get<double>(parameters[14].data), 0.6);
    EXPECT_DOUBLE_EQ(std::get<double>(parameters[15].data), 0.2);
    memory.reportedDepth = 0;
    BindMemory(statement, {ActorKind::Player, 1}, memory);
    EXPECT_EQ(std::get<uint32>(parameters[13].data), 0u);
}

TEST(AllesSnapshotStatementsTest, LanguageGatedTextAndRuntimeReceiptsStayOutOfStorage)
{
    Perception perception;
    perception.id = LargeId;
    perception.comprehended = false;
    perception.text = "foreign private information";
    perception.language = 7;
    perception.gameTimeMs = LargeId + 1;
    perception.admittedRealTimeMs = 998;
    perception.emissionId = 999;
    ASSERT_TRUE(GatePerception(perception));
    CharacterDatabasePreparedStatement statement(CHAR_INS_ALLES_PERCEPTION, 16);
    BindPerception(statement, {ActorKind::Player, 1}, perception);
    auto const& parameters = statement.GetParameters();
    ASSERT_EQ(parameters.size(), 16u);
    EXPECT_EQ(std::get<uint64>(parameters[2].data), LargeId);
    EXPECT_EQ(std::get<uint8>(parameters[10].data), 0u);
    EXPECT_EQ(std::get<uint32>(parameters[11].data), 7u);
    EXPECT_TRUE(std::get<std::string>(parameters[12].data).empty());
    EXPECT_EQ(std::get<uint64>(parameters[14].data), LargeId + 1);
}

TEST(AllesSnapshotStatementsTest, UnicodePreparedParametersOwnTheirBytes)
{
    Memory memory;
    for (std::size_t index = 0; index < 512; ++index)
        memory.claim += "\xF0\x9F\x90\xBA";
    auto const original = memory.claim;
    CharacterDatabasePreparedStatement statement(CHAR_INS_ALLES_MEMORY, 23);
    BindMemory(statement, {ActorKind::Player, 1}, memory);
    memory.claim.clear();
    EXPECT_EQ(std::get<std::string>(statement.GetParameters()[11].data), original);
}

TEST(AllesSnapshotStatementsTest, InvalidSnapshotsAreRejectedBeforeDatabaseAllocation)
{
    SnapshotDao dao;
    SaveRequest request;
    request.generation = 1;
    request.snapshot.owner = {ActorKind::Player, 1};
    request.snapshot.revision = 1;
    request.snapshot.nextPerceptionId = 2;
    Perception perception;
    perception.id = 1;
    perception.comprehended = false;
    perception.text = "unremoved foreign plaintext";
    request.snapshot.perceptions.push_back(perception);
    EXPECT_FALSE(dao.StartSave(request));
    EXPECT_EQ(dao.PendingSaves(), 0u);
    EXPECT_FALSE(dao.StartLoad({ActorKind::Player, 0}, 1));
    EXPECT_FALSE(dao.StartLoad({ActorKind::Player, 1}, 0));
    dao.StopAsync();
    EXPECT_FALSE(dao.StartLoad({ActorKind::Player, 1}, 1));
    EXPECT_EQ(dao.PendingLoads(), 0u);
}
}

TEST(AllesSnapshotStatementsTest, FamiliarityBindsIdentityCountTimeAndPlaceWithoutChangingProvenance)
{
    Memory memory;
    memory.kind = MemoryKind::Met;
    memory.encounters = 45;
    memory.lastSeenGameTimeMs = LargeId;
    memory.lastSeenPlace = "A changed location";
    CharacterDatabasePreparedStatement statement(CHAR_INS_ALLES_MEMORY, 23);
    BindMemory(statement, {ActorKind::Player, 1}, memory);
    auto const& parameters = statement.GetParameters();
    EXPECT_EQ(std::get<uint32>(parameters[20].data), 45u);
    EXPECT_EQ(std::get<uint64>(parameters[21].data), LargeId);
    EXPECT_EQ(std::get<std::string>(parameters[22].data), memory.lastSeenPlace);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(parameters[13].data));
}

}
