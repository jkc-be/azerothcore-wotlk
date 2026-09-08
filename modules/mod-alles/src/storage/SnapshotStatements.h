/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_SNAPSHOT_STATEMENTS_H
#define MOD_ALLES_SNAPSHOT_STATEMENTS_H

#include "ActorStore.h"
#include "CharacterDatabase.h"
#include "PreparedStatement.h"

namespace Alles::Storage
{
// Binding helpers also permit testing actual prepared parameters without opening a database.
void BindOwner(CharacterDatabasePreparedStatement& statement, ActorKey owner);
void BindActor(CharacterDatabasePreparedStatement& statement, OwnerSnapshot const& snapshot);
void BindPerception(CharacterDatabasePreparedStatement& statement, ActorKey owner, Perception const& perception);
void BindMemory(CharacterDatabasePreparedStatement& statement, ActorKey owner, Memory const& memory);
void BindPlanning(CharacterDatabasePreparedStatement& statement, PlanningSnapshot const& snapshot);
}

#endif
