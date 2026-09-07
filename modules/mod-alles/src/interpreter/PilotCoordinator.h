/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_PILOT_COORDINATOR_H
#define MOD_ALLES_PILOT_COORDINATOR_H

#include "ActorStore.h"
#include <memory>

namespace Alles::Interpreter
{
struct PerceptionInput
{
    std::string token;
    Perception value;
    std::string subjectToken;
    std::string sourceToken;
    std::string placeToken;
};

struct MemoryInput
{
    std::string token;
    Memory value;
};

struct EntityInput
{
    std::string token;
    Reference value;
};

struct PlaceInput
{
    std::string token;
    std::string name;
};

struct JobSnapshot
{
    std::string bootEpoch;
    std::string jobToken;
    ActorKey owner;
    uint64_t actorGeneration = 0;
    std::string workerId;
    std::string profileFingerprint;
    uint64_t leaseGeneration = 0;
    std::string permitId;
    uint32_t httpAttemptCount = 0; // Fake permits never represent a provider HTTP attempt.
    uint64_t admittedRealTimeMs = 0;
    uint64_t admittedGameTimeMs = 0;
    uint64_t leaseExpiresRealTimeMs = 0;
    uint64_t permitExpiresRealTimeMs = 0;
    std::size_t contextBytesBound = 0;
    std::vector<PerceptionInput> perceptions;
    std::vector<MemoryInput> memories;
    std::vector<EntityInput> entities;
    std::vector<PlaceInput> places;
};

enum class ProposalOperation : uint8_t
{
    Create,
    Reinforce,
    Supersede
};

struct MemoryProposal
{
    ProposalOperation operation = ProposalOperation::Create;
    std::vector<std::string> supportingPerceptions;
    std::string targetMemoryToken;
    std::string subjectToken;
    std::string sourceToken;
    std::string placeToken;
    std::string text;
    double confidence = 0;
    double salience = 0;
    MemoryKind kind = MemoryKind::HeardStatement;
};

struct ProposalEnvelope
{
    std::string bootEpoch;
    std::string jobToken;
    ActorKey owner;
    uint64_t actorGeneration = 0;
    std::string workerId;
    std::string profileFingerprint;
    uint64_t leaseGeneration = 0;
    std::string permitId;
    std::vector<MemoryProposal> memories;
};

enum class FakeMode : uint8_t
{
    Valid,
    Malformed,
    Stale,
    Withhold
};

struct FakeBehavior
{
    FakeMode mode = FakeMode::Valid;
    uint64_t delayMs = 0;
    bool heartbeats = true;
};

struct CoordinatorLimits
{
    std::size_t owners = 256;
    std::size_t jobs = 64;
    std::size_t resultQueue = 64;
    std::size_t concurrentLeases = 1;
};

struct CoordinatorStats
{
    uint64_t dispatched = 0;
    uint64_t fakeMemories = 0;
    uint64_t fakeReinforcements = 0;
    uint64_t fakeSupersessions = 0;
    uint64_t fallbackMemories = 0;
    uint64_t reflexMemories = 0;
    uint64_t invalidResults = 0;
    uint64_t invalidatedJobs = 0;
    uint64_t staleResults = 0;
    uint64_t expiredJobs = 0;
    uint64_t contextOverflows = 0;
    uint64_t resultDrops = 0;
    uint64_t modelMemories = 0;
};

// The C++ fake produces values through exactly the same acceptance queue as external proposals.
ProposalEnvelope MakeFakeProposal(JobSnapshot const& job, MemoryPolicy const& policy);

class PilotCoordinator
{
public:
    explicit PilotCoordinator(ActorStore& store, MemoryPolicy policy = {}, CoordinatorLimits limits = {});
    ~PilotCoordinator();
    PilotCoordinator(PilotCoordinator const&) = delete;
    PilotCoordinator& operator=(PilotCoordinator const&) = delete;

    bool Track(ActorKey owner);
    bool Forget(ActorKey owner);
    void Update(uint64_t gameTimeMs, uint64_t realTimeMs, std::size_t itemBudget = 32);
    bool Submit(ProposalEnvelope result);
    void SetFakeBehavior(FakeBehavior behavior);
    std::optional<JobSnapshot> Inspect(ActorKey owner) const;
    CoordinatorStats const& Stats() const;

    void EnableExternal();
    std::optional<JobSnapshot> Claim(std::string worker, std::string profile, uint64_t gameMs, uint64_t realMs);
    bool Heartbeat(std::string const& token, std::string const& worker, uint64_t lease, uint64_t realMs);
    bool Authorize(std::string const& token, std::string const& worker, uint64_t lease,
        std::string permit, uint64_t gameMs, uint64_t realMs);
    bool Release(std::string const& token, std::string const& worker, uint64_t lease);
    std::string ApplyExternal(ProposalEnvelope const& result, uint64_t gameMs, uint64_t realMs);

    // Stop fences/discards unfinished fake results. Village shutdown may then explicitly template
    // ordered pending inputs; strict labs must retain them instead of calling DrainFallback.
    void Stop();
    std::size_t DrainFallback(uint64_t gameTimeMs, uint64_t realTimeMs, std::size_t itemBudget = 32);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}

#endif
