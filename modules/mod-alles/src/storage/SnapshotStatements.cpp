/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "SnapshotStatements.h"
#include "PlanningCodec.h"

namespace Alles::Storage
{
namespace
{
void BindReference(CharacterDatabasePreparedStatement& statement, uint8 offset, Reference const& reference)
{
    if (reference.actor)
    {
        statement.SetData(offset, reference.actor->kind);
        statement.SetData(offset + 1, reference.actor->id);
    }
    else
    {
        statement.SetData(offset, nullptr);
        statement.SetData(offset + 1, nullptr);
    }
    statement.SetData(offset + 2, std::string_view(reference.name));
}
}

void BindOwner(CharacterDatabasePreparedStatement& statement, ActorKey owner)
{
    statement.SetArguments(owner.kind, owner.id);
}

void BindActor(CharacterDatabasePreparedStatement& statement, OwnerSnapshot const& snapshot)
{
    statement.SetArguments(snapshot.owner.kind, snapshot.owner.id, snapshot.revision, snapshot.nextPerceptionId,
        snapshot.nextMemoryId, snapshot.decayGameTimeMs, snapshot.nextConsolidationGameTimeMs);
}

void BindPlanning(CharacterDatabasePreparedStatement& statement, PlanningSnapshot const& snapshot)
{
    auto const payload = EncodePlanning(snapshot);
    statement.SetArguments(snapshot.owner.kind, snapshot.owner.id, std::string_view(payload));
}

void BindPerception(CharacterDatabasePreparedStatement& statement, ActorKey owner, Perception const& perception)
{
    BindOwner(statement, owner);
    statement.SetData(2, perception.id);
    statement.SetData(3, perception.kind);
    BindReference(statement, 4, perception.subject);
    BindReference(statement, 7, perception.source);
    statement.SetData(10, uint8(perception.comprehended));
    statement.SetData(11, perception.language);
    statement.SetData(12, std::string_view(perception.text));
    statement.SetData(13, std::string_view(perception.place));
    statement.SetData(14, perception.gameTimeMs);
    statement.SetData(15, std::string_view(perception.selfContext));
}

void BindMemory(CharacterDatabasePreparedStatement& statement, ActorKey owner, Memory const& memory)
{
    BindOwner(statement, owner);
    statement.SetData(2, memory.id);
    statement.SetData(3, memory.contentRevision);
    statement.SetData(4, memory.kind);
    BindReference(statement, 5, memory.subject);
    BindReference(statement, 8, memory.source);
    statement.SetData(11, std::string_view(memory.claim));
    statement.SetData(12, std::string_view(memory.attribution));
    if (memory.reportedDepth)
        statement.SetData(13, *memory.reportedDepth);
    else
        statement.SetData(13, nullptr);
    statement.SetData(14, memory.confidence);
    statement.SetData(15, memory.salience);
    statement.SetData(16, memory.formedGameTimeMs);
    statement.SetData(17, memory.recalledGameTimeMs);
    statement.SetData(18, memory.decayGameTimeMs);
    statement.SetData(19, memory.formation);
    statement.SetData(20, memory.encounters);
    statement.SetData(21, memory.lastSeenGameTimeMs);
    statement.SetData(22, std::string_view(memory.lastSeenPlace));
}
}
