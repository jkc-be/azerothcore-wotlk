/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "SnapshotDao.h"
#include "PlanningCodec.h"
#include "AllesDatabaseGuard.h"
#include "DatabaseEnv.h"
#include "Errors.h"
#include "Field.h"
#include "Log.h"
#include "QueryHolder.h"
#include "QueryResult.h"
#include "SnapshotStatements.h"
#include <array>
#include <initializer_list>
#include <limits>
#include <map>
#include <thread>

namespace Alles::Storage
{
namespace
{
enum LoadQuery : std::size_t
{
    ActorBefore,
    Perceptions,
    Memories,
    Planning,
    ActorAfter,
    LoadQueryCount
};

bool AllNull(Field const* fields, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
        if (!fields[index].IsNull())
            return false;
    return true;
}

bool RequiredFields(Field const* fields, std::initializer_list<std::size_t> indices)
{
    for (auto index : indices)
        if (fields[index].IsNull())
            return false;
    return true;
}

bool ReadReference(Field const* fields, Reference& reference)
{
    if (fields[0].IsNull() != fields[1].IsNull() || fields[2].IsNull())
        return false;
    if (!fields[0].IsNull())
        reference.actor = ActorKey{ActorKind(fields[0].Get<uint8>()), fields[1].Get<uint64>()};
    reference.name = fields[2].Get<std::string>();
    return true;
}

bool ReadActor(PreparedQueryResult const& result, OwnerSnapshot& snapshot, bool& exists)
{
    if (result->GetRowCount() != 1 || result->GetFieldCount() != 6)
        return false;
    auto const* fields = result->Fetch();
    exists = !fields[0].IsNull();
    if (!exists)
        return AllNull(fields, 6);
    if (!RequiredFields(fields, {0, 1, 2, 3, 4, 5}) || fields[0].Get<uint64>() != snapshot.owner.id)
        return false;
    snapshot.revision = fields[1].Get<uint64>();
    snapshot.nextPerceptionId = fields[2].Get<uint64>();
    snapshot.nextMemoryId = fields[3].Get<uint64>();
    snapshot.decayGameTimeMs = fields[4].Get<uint64>();
    snapshot.nextConsolidationGameTimeMs = fields[5].Get<uint64>();
    return true;
}

bool ReadPerceptions(PreparedQueryResult const& result, OwnerSnapshot& snapshot, std::size_t cap)
{
    if (result->GetFieldCount() != 14)
        return false;
    if (result->Fetch()[0].IsNull())
        return result->GetRowCount() == 1 && AllNull(result->Fetch(), 14);
    if (result->GetRowCount() > cap)
        return false;
    do
    {
        auto const* fields = result->Fetch();
        if (!RequiredFields(fields, {0, 1, 8, 9, 10, 11, 12, 13}) || fields[8].Get<uint8>() > 1)
            return false;
        Perception perception;
        perception.id = fields[0].Get<uint64>();
        perception.kind = PerceptionKind(fields[1].Get<uint8>());
        if (!ReadReference(fields + 2, perception.subject) || !ReadReference(fields + 5, perception.source))
            return false;
        perception.comprehended = fields[8].Get<uint8>() != 0;
        perception.language = fields[9].Get<uint32>();
        perception.text = fields[10].Get<std::string>();
        perception.place = fields[11].Get<std::string>();
        perception.gameTimeMs = fields[12].Get<uint64>();
        perception.selfContext = fields[13].Get<std::string>();
        snapshot.perceptions.push_back(std::move(perception));
    } while (result->NextRow());
    return true;
}

bool ReadMemories(PreparedQueryResult const& result, OwnerSnapshot& snapshot, std::size_t cap)
{
    if (result->GetFieldCount() != 18)
        return false;
    if (result->Fetch()[0].IsNull())
        return result->GetRowCount() == 1 && AllNull(result->Fetch(), 18);
    if (result->GetRowCount() > cap)
        return false;
    do
    {
        auto const* fields = result->Fetch();
        if (!RequiredFields(fields, {0, 1, 2, 9, 10, 12, 13, 14, 15, 16, 17}))
            return false;
        Memory memory;
        memory.id = fields[0].Get<uint64>();
        memory.contentRevision = fields[1].Get<uint64>();
        memory.kind = MemoryKind(fields[2].Get<uint8>());
        if (!ReadReference(fields + 3, memory.subject) || !ReadReference(fields + 6, memory.source))
            return false;
        memory.claim = fields[9].Get<std::string>();
        memory.attribution = fields[10].Get<std::string>();
        if (!fields[11].IsNull())
            memory.reportedDepth = fields[11].Get<uint32>();
        memory.confidence = fields[12].Get<double>();
        memory.salience = fields[13].Get<double>();
        memory.formedGameTimeMs = fields[14].Get<uint64>();
        memory.recalledGameTimeMs = fields[15].Get<uint64>();
        memory.decayGameTimeMs = fields[16].Get<uint64>();
        memory.formation = FormationMode(fields[17].Get<uint8>());
        snapshot.memories.push_back(std::move(memory));
    } while (result->NextRow());
    return true;
}

bool ReadPlanning(PreparedQueryResult const& result, OwnerSnapshot& snapshot)
{
    if (result->GetRowCount() != 1 || result->GetFieldCount() != 2)
        return false;
    auto const* fields = result->Fetch();
    if (fields[0].IsNull())
        return AllNull(fields, 2);
    if (fields[0].Get<uint64>() != snapshot.owner.id || fields[1].IsNull())
        return false;
    snapshot.planning = DecodePlanning(fields[1].Get<std::string>(), snapshot.owner);
    return snapshot.planning.has_value();
}

LoadResult DecodeLoad(ActorKey owner, uint64_t generation, SQLQueryHolderBase const& holder,
    StoreLimits const& limits, MemoryPolicy const& policy)
{
    LoadResult result{owner, generation, LoadOutcome::QueryFailed, std::nullopt};
    std::array<PreparedQueryResult, LoadQueryCount> rows;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        rows[index] = holder.GetPreparedResult(index);
        // Successful empty-table queries contain a sentinel row; null is always an error.
        if (!rows[index])
            return result;
    }

    result.outcome = LoadOutcome::InvalidSnapshot;
    OwnerSnapshot before;
    OwnerSnapshot after;
    before.owner = owner;
    after.owner = owner;
    bool existedBefore = false;
    bool existsAfter = false;
    if (!ReadActor(rows[ActorBefore], before, existedBefore) || !ReadActor(rows[ActorAfter], after, existsAfter))
        return result;

    // A sole writer commits actor metadata and children atomically with a monotonically increasing revision.
    // Identical-revision retries contain the same snapshot. An interleaved different commit is fenced here.
    if (existedBefore != existsAfter || before.revision != after.revision
        || before.nextMemoryId != after.nextMemoryId || before.nextPerceptionId != after.nextPerceptionId
        || before.decayGameTimeMs != after.decayGameTimeMs
        || before.nextConsolidationGameTimeMs != after.nextConsolidationGameTimeMs)
    {
        result.outcome = LoadOutcome::RevisionChanged;
        return result;
    }
    if (!ReadPerceptions(rows[Perceptions], before, limits.perceptions)
        || !ReadMemories(rows[Memories], before, limits.memories)
        || !ReadPlanning(rows[Planning], before)
        || (!existedBefore && (!before.perceptions.empty() || !before.memories.empty() || before.planning))
        || !IsValidSnapshot(before, limits, policy))
        return result;

    result.outcome = existedBefore ? LoadOutcome::Loaded : LoadOutcome::Missing;
    result.snapshot = std::move(before);
    return result;
}

CharacterDatabaseTransaction BuildTransaction(OwnerSnapshot const& snapshot)
{
    auto transaction = CharacterDatabase.BeginTransaction();
    for (auto index : {CHAR_DEL_ALLES_PERCEPTIONS, CHAR_DEL_ALLES_MEMORIES})
    {
        auto* statement = CharacterDatabase.GetPreparedStatement(index);
        BindOwner(*statement, snapshot.owner);
        transaction->Append(statement);
    }
    for (auto const& perception : snapshot.perceptions)
    {
        auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_INS_ALLES_PERCEPTION);
        BindPerception(*statement, snapshot.owner, perception);
        transaction->Append(statement);
    }
    for (auto const& memory : snapshot.memories)
    {
        auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_INS_ALLES_MEMORY);
        BindMemory(*statement, snapshot.owner, memory);
        transaction->Append(statement);
    }
    if (snapshot.planning)
    {
        auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_REP_ALLES_PLANNING);
        BindPlanning(*statement, *snapshot.planning);
        transaction->Append(statement);
    }
    else
    {
        auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ALLES_PLANNING);
        BindOwner(*statement, snapshot.owner);
        transaction->Append(statement);
    }
    auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_REP_ALLES_ACTOR);
    BindActor(*statement, snapshot);
    transaction->Append(statement);
    return transaction;
}
}

struct SnapshotDao::Impl
{
    struct PendingLoad
    {
        uint64_t generation;
        SQLQueryHolderCallback callback;
    };

    struct PendingSave
    {
        uint64_t generation;
        uint64_t revision;
        TransactionCallback callback;
    };

    Impl(StoreLimits storeLimits, MemoryPolicy memoryPolicy) : limits(storeLimits), policy(memoryPolicy) { }

    void CheckThread() const
    {
        if (thread != std::this_thread::get_id())
            ABORT("Alles SnapshotDao must be used on its owning world thread");
    }

    static SaveResult ConsumeSave(ActorKey owner, PendingSave& save)
    {
        SaveResult result{owner, save.generation, save.revision, SaveOutcome::Failed};
        try
        {
            if (save.callback.m_future.get())
                result.outcome = SaveOutcome::Committed;
        }
        catch (std::future_error const& error)
        {
            LOG_ERROR("module.alles", "Snapshot transaction future failed: {}", error.what());
        }
        return result;
    }

    StoreLimits limits;
    MemoryPolicy policy;
    std::thread::id thread = std::this_thread::get_id();
    std::map<ActorKey, PendingLoad> loads;
    std::map<ActorKey, PendingSave> saves;
    bool stopping = false;
};

SnapshotDao::SnapshotDao(StoreLimits limits, MemoryPolicy policy) : _impl(std::make_unique<Impl>(limits, policy))
{
    CheckCharacterStatementLayout();
    if (!limits.owners || !limits.inFlightSaves || limits.inFlightSaves > 4 || !limits.memories || !limits.perceptions
        || limits.memories >= std::numeric_limits<uint32>::max()
        || limits.perceptions >= std::numeric_limits<uint32>::max() || !IsValidPolicy(policy))
        ABORT("Invalid Alles SnapshotDao limits or memory policy");
}

SnapshotDao::~SnapshotDao()
{
    _impl->CheckThread();
    if (!_impl->saves.empty() || !_impl->loads.empty())
        LOG_ERROR("module.alles", "SnapshotDao destroyed with {} unresolved saves and {} unresolved loads; "
            "these owners must not be reported durable or evicted", _impl->saves.size(), _impl->loads.size());
}

bool SnapshotDao::StartLoad(ActorKey owner, uint64_t generation)
{
    _impl->CheckThread();
    if (_impl->stopping || !generation || !IsValidActor(owner) || _impl->loads.contains(owner)
        || _impl->saves.contains(owner) || _impl->loads.size() >= _impl->limits.inFlightSaves)
        return false;
    auto holder = std::make_shared<SQLQueryHolder<CharacterDatabaseConnection>>();
    holder->SetSize(LoadQueryCount);
    constexpr std::array<CharacterDatabaseStatements, LoadQueryCount> queries =
        {CHAR_SEL_ALLES_ACTOR, CHAR_SEL_ALLES_PERCEPTIONS, CHAR_SEL_ALLES_MEMORIES,
            CHAR_SEL_ALLES_PLANNING, CHAR_SEL_ALLES_ACTOR};
    for (std::size_t index = 0; index < queries.size(); ++index)
    {
        auto* statement = CharacterDatabase.GetPreparedStatement(queries[index]);
        BindOwner(*statement, owner);
        if (index == Perceptions)
            statement->SetData(2, uint32(_impl->limits.perceptions + 1));
        else if (index == Memories)
            statement->SetData(2, uint32(_impl->limits.memories + 1));
        if (!holder->SetPreparedQuery(index, statement))
        {
            delete statement;
            return false;
        }
    }
    _impl->loads.emplace(owner, Impl::PendingLoad{generation, CharacterDatabase.DelayQueryHolder(std::move(holder))});
    return true;
}

bool SnapshotDao::StartSave(SaveRequest const& request)
{
    _impl->CheckThread();
    auto const owner = request.snapshot.owner;
    if (_impl->stopping || !request.generation || !request.snapshot.revision
        || !IsValidSnapshot(request.snapshot, _impl->limits, _impl->policy)
        || _impl->saves.contains(owner) || _impl->loads.contains(owner)
        || _impl->saves.size() >= _impl->limits.inFlightSaves)
        return false;
    _impl->saves.emplace(owner, Impl::PendingSave{request.generation, request.snapshot.revision,
        CharacterDatabase.AsyncCommitTransaction(BuildTransaction(request.snapshot))});
    return true;
}

std::vector<Completion> SnapshotDao::Poll(std::size_t resultBudget)
{
    _impl->CheckThread();
    std::vector<Completion> results;
    for (auto iterator = _impl->saves.begin(); iterator != _impl->saves.end() && results.size() < resultBudget;)
    {
        if (iterator->second.callback.m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }
        results.emplace_back(Impl::ConsumeSave(iterator->first, iterator->second));
        iterator = _impl->saves.erase(iterator);
    }
    for (auto iterator = _impl->loads.begin(); iterator != _impl->loads.end() && results.size() < resultBudget;)
    {
        auto& pending = iterator->second;
        if (pending.callback.m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }
        try
        {
            pending.callback.m_future.get();
            results.emplace_back(DecodeLoad(iterator->first, pending.generation, *pending.callback.m_holder,
                _impl->limits, _impl->policy));
        }
        catch (std::future_error const& error)
        {
            LOG_ERROR("module.alles", "Snapshot load future failed: {}", error.what());
            results.emplace_back(LoadResult{iterator->first, pending.generation,
                LoadOutcome::QueryFailed, std::nullopt});
        }
        iterator = _impl->loads.erase(iterator);
    }
    return results;
}

void SnapshotDao::StopAsync()
{
    _impl->CheckThread();
    _impl->stopping = true;
}

std::vector<SaveResult> SnapshotDao::DrainOlderWrites(Deadline deadline)
{
    _impl->CheckThread();
    _impl->stopping = true;
    std::vector<SaveResult> results;
    for (auto iterator = _impl->saves.begin(); iterator != _impl->saves.end();)
    {
        auto& pending = iterator->second;
        if (pending.callback.m_future.wait_until(deadline) != std::future_status::ready)
        {
            results.push_back({iterator->first, pending.generation, pending.revision,
                SaveOutcome::UndrainedOlderWrite, true});
            ++iterator;
            continue;
        }
        results.push_back(Impl::ConsumeSave(iterator->first, pending));
        iterator = _impl->saves.erase(iterator);
    }
    return results;
}

SaveResult SnapshotDao::CommitFinal(SaveRequest const& request, Deadline deadline)
{
    _impl->CheckThread();
    SaveResult result{request.snapshot.owner, request.generation, request.snapshot.revision,
        SaveOutcome::InvalidRequest};
    if (!_impl->stopping || !request.generation || !request.snapshot.revision
        || !IsValidSnapshot(request.snapshot, _impl->limits, _impl->policy))
        return result;
    if (_impl->saves.contains(request.snapshot.owner) || _impl->loads.contains(request.snapshot.owner))
    {
        result.outcome = SaveOutcome::UndrainedOlderWrite;
        return result;
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
        result.outcome = SaveOutcome::DeadlineExpired;
        result.budgetExceeded = true;
        return result;
    }

    auto transaction = BuildTransaction(request.snapshot);
    CharacterDatabase.DirectCommitTransaction(transaction);
    if (std::chrono::steady_clock::now() >= deadline)
    {
        // The write may have committed, but its void return is not a durability acknowledgment.
        result.outcome = SaveOutcome::DeadlineExpired;
        result.budgetExceeded = true;
        return result;
    }
    auto* statement = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ALLES_COMMITTED_REVISION);
    BindOwner(*statement, request.snapshot.owner);
    auto rows = CharacterDatabase.Query(statement);
    if (!rows || rows->GetRowCount() != 1 || rows->GetFieldCount() != 1 || rows->Fetch()[0].IsNull())
        result.outcome = SaveOutcome::ReadbackFailed;
    else
        result.outcome = rows->Fetch()[0].Get<uint64>() == request.snapshot.revision
            ? SaveOutcome::Committed : SaveOutcome::RevisionMismatch;
    result.budgetExceeded = std::chrono::steady_clock::now() >= deadline;
    return result;
}

std::size_t SnapshotDao::PendingLoads() const
{
    _impl->CheckThread();
    return _impl->loads.size();
}

std::size_t SnapshotDao::PendingSaves() const
{
    _impl->CheckThread();
    return _impl->saves.size();
}
}
