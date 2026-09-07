/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_SNAPSHOT_DAO_H
#define MOD_ALLES_SNAPSHOT_DAO_H

#include "ActorStore.h"
#include <chrono>
#include <memory>
#include <variant>

namespace Alles::Storage
{
enum class LoadOutcome
{
    Loaded,
    Missing,
    QueryFailed,
    RevisionChanged,
    InvalidSnapshot
};

struct LoadResult
{
    ActorKey owner;
    uint64_t generation;
    LoadOutcome outcome;
    std::optional<OwnerSnapshot> snapshot;
};

enum class SaveOutcome
{
    Committed,
    Failed,
    ReadbackFailed,
    RevisionMismatch,
    UndrainedOlderWrite,
    DeadlineExpired,
    InvalidRequest
};

struct SaveResult
{
    ActorKey owner;
    uint64_t generation;
    uint64_t revision;
    SaveOutcome outcome;
    bool budgetExceeded = false;
};

using Completion = std::variant<LoadResult, SaveResult>;

// Construct and use on the world thread; keep alive through final map/session teardown.
// No completion mutates ActorStore. The caller applies generation/revision-fenced value results.
class SnapshotDao
{
public:
    using Deadline = std::chrono::steady_clock::time_point;

    explicit SnapshotDao(StoreLimits limits = {}, MemoryPolicy policy = {});
    ~SnapshotDao();
    SnapshotDao(SnapshotDao const&) = delete;
    SnapshotDao& operator=(SnapshotDao const&) = delete;

    // False means nothing was submitted; the caller retains/retries its request.
    bool StartLoad(ActorKey owner, uint64_t generation);
    bool StartSave(SaveRequest const& request);
    std::vector<Completion> Poll(std::size_t resultBudget = 32);

    void StopAsync();
    // Retains timed-out futures. Their owners cannot be overtaken by CommitFinal.
    std::vector<SaveResult> DrainOlderWrites(Deadline deadline);
    // Requires StopAsync and no outstanding read/write for this owner. The deadline prevents
    // starting further calls; the core's synchronous connection acquisition cannot be cancelled.
    SaveResult CommitFinal(SaveRequest const& request, Deadline deadline);
    std::size_t PendingLoads() const;
    std::size_t PendingSaves() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}

#endif
