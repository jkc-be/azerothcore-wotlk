/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PilotCoordinator.h"
#include "CryptoRandom.h"
#include "Errors.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <thread>

namespace Alles::Interpreter
{
namespace
{
constexpr uint64_t DebounceMs = 5000;
constexpr uint64_t OwnerTokenIntervalMs = 15000;
constexpr uint64_t RealDeadlineMs = 45000;
constexpr uint64_t GameDeadlineMs = 120000;
constexpr uint64_t LeaseMs = 15000;
constexpr uint64_t HeartbeatMs = 5000;
constexpr uint64_t FakePermitMs = 25000;
constexpr std::size_t ContextLimit = 24 * 1024;
constexpr std::size_t BatchLimit = 8;
constexpr std::size_t OutputLimit = 4;
constexpr std::size_t MemoryLimit = 12;
constexpr char FakeWorkerId[] = "inprocess-fake";
constexpr char FakeProfile[] = "alles-inprocess-fake-pilot-v1";

uint64_t AddTime(uint64_t start, uint64_t duration)
{
    return duration > std::numeric_limits<uint64_t>::max() - start
        ? std::numeric_limits<uint64_t>::max() : start + duration;
}

bool Elapsed(uint64_t now, uint64_t then, uint64_t duration)
{
    return now >= then && now - then >= duration;
}

std::string RandomToken()
{
    auto const bytes = Acore::Crypto::GetRandomBytes<16>();
    constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (auto byte : bytes)
    {
        token += digits[byte >> 4];
        token += digits[byte & 15];
    }
    return token;
}

std::size_t TextBound(std::string const& text)
{
    std::size_t bytes = 2;
    for (unsigned char byte : text)
        bytes += byte < 0x20 ? 6 : (byte == '"' || byte == '\\' ? 2 : 1);
    return bytes;
}

std::size_t ReferenceBound(Reference const& reference)
{
    return 128 + TextBound(reference.name);
}

std::size_t ContextBound(JobSnapshot const& job)
{
    std::size_t bytes = 1024; // Bounded identity/lease metadata and stable fake-contract overhead.
    for (auto const& input : job.perceptions)
    {
        auto const& value = input.value;
        bytes += 512 + TextBound(input.token) + TextBound(input.subjectToken) + TextBound(input.sourceToken)
            + TextBound(input.placeToken) + ReferenceBound(value.subject) + ReferenceBound(value.source)
            + TextBound(value.text) + TextBound(value.place) + TextBound(value.selfContext);
    }
    for (auto const& input : job.memories)
        bytes += 512 + TextBound(input.token) + TextBound(input.value.claim) + TextBound(input.value.attribution)
            + ReferenceBound(input.value.subject) + ReferenceBound(input.value.source);
    for (auto const& entity : job.entities)
        bytes += 64 + TextBound(entity.token) + ReferenceBound(entity.value);
    for (auto const& place : job.places)
        bytes += 64 + TextBound(place.token) + TextBound(place.name);
    return bytes;
}

std::string AddEntity(JobSnapshot& job, Reference const& reference)
{
    if (!reference.actor && reference.name.empty())
        return {};
    auto const existing = std::find_if(job.entities.begin(), job.entities.end(), [&reference](EntityInput const& input)
        { return input.value == reference; });
    if (existing != job.entities.end())
        return existing->token;
    std::string token = "e" + std::to_string(job.entities.size() + 1);
    job.entities.push_back({token, reference});
    return token;
}

std::string AddPlace(JobSnapshot& job, std::string const& place)
{
    if (place.empty())
        return {};
    auto const existing = std::find_if(job.places.begin(), job.places.end(), [&place](PlaceInput const& input)
        { return input.name == place; });
    if (existing != job.places.end())
        return existing->token;
    std::string token = "l" + std::to_string(job.places.size() + 1);
    job.places.push_back({token, place});
    return token;
}

struct FormationGroup
{
    Memory memory;
    std::vector<std::size_t> inputs;
};

bool Compatible(Perception const& first, Perception const& next, Memory const& left, Memory const& right)
{
    return left.kind == right.kind && left.subject == right.subject && left.source == right.source
        && left.attribution == right.attribution && left.reportedDepth == right.reportedDepth
        && first.place == next.place && first.language == next.language && first.comprehended == next.comprehended;
}

std::vector<FormationGroup> GroupInputs(std::vector<Perception> const& inputs, MemoryPolicy const& policy,
    uint64_t gameMs)
{
    std::vector<FormationGroup> groups;
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        auto memory = FormFallback(inputs[index], policy, gameMs);
        if (!groups.empty())
        {
            auto& previous = groups.back();
            auto const combined = previous.memory.claim + "; " + memory.claim;
            if (Compatible(inputs[previous.inputs.front()], inputs[index], previous.memory, memory)
                && IsBoundedText(combined, 512))
            {
                previous.memory.claim = combined;
                previous.memory.confidence = std::min(previous.memory.confidence, memory.confidence);
                previous.memory.salience = std::max(previous.memory.salience, memory.salience);
                previous.inputs.push_back(index);
                continue;
            }
        }
        if (groups.size() >= OutputLimit)
            break;
        groups.push_back({std::move(memory), {index}});
    }
    return groups;
}

std::vector<Perception> InputValues(JobSnapshot const& job)
{
    std::vector<Perception> values;
    for (auto const& input : job.perceptions)
        values.push_back(input.value);
    return values;
}

bool DeadlineExpired(JobSnapshot const& job, uint64_t gameMs, uint64_t realMs)
{
    return Elapsed(realMs, job.admittedRealTimeMs, RealDeadlineMs)
        || Elapsed(gameMs, job.admittedGameTimeMs, GameDeadlineMs);
}
}

ProposalEnvelope MakeFakeProposal(JobSnapshot const& job, MemoryPolicy const& policy)
{
    ProposalEnvelope result{job.bootEpoch, job.jobToken, job.owner, job.actorGeneration, job.workerId,
        job.profileFingerprint, job.leaseGeneration, job.permitId, {}};
    for (auto& group : GroupInputs(InputValues(job), policy, job.admittedGameTimeMs))
    {
        auto const& first = job.perceptions[group.inputs.front()];
        MemoryProposal proposal{ProposalOperation::Create, {}, {}, first.subjectToken,
            first.sourceToken, first.placeToken, std::move(group.memory.claim),
            group.memory.confidence, group.memory.salience};
        proposal.kind = group.memory.kind;
        for (auto index : group.inputs)
            proposal.supportingPerceptions.push_back(job.perceptions[index].token);
        result.memories.push_back(std::move(proposal));
    }
    return result;
}

struct PilotCoordinator::Impl
{
    struct Owner
    {
        uint64_t nextTokenRealMs = 0;
    };

    struct Job
    {
        JobSnapshot snapshot;
        bool leased = false;
        FakeBehavior behavior;
        uint64_t nextHeartbeatRealMs = 0;
        uint64_t resultDueRealMs = 0;
        std::optional<ProposalEnvelope> fakeResult;
    };

    Impl(ActorStore& ownerStore, MemoryPolicy memoryPolicy, CoordinatorLimits coordinatorLimits)
        : store(ownerStore), policy(memoryPolicy), limits(coordinatorLimits), bootEpoch(RandomToken()) { }

    void CheckThread() const
    {
        if (thread != std::this_thread::get_id())
            ABORT("Alles PilotCoordinator must be used on its owning world thread");
    }

    bool PrefixCurrent(JobSnapshot const& job) const
    {
        auto const status = store.Status(job.owner);
        auto const* current = store.FindReady(job.owner);
        if (!status || status->generation != job.actorGeneration || !current
            || current->perceptions.size() < job.perceptions.size())
            return false;
        for (std::size_t index = 0; index < job.perceptions.size(); ++index)
            if (current->perceptions[index].id != job.perceptions[index].value.id)
                return false;
        return true;
    }

    bool MemoryContextCurrent(JobSnapshot const& job) const
    {
        auto const* current = store.FindReady(job.owner);
        if (!current)
            return false;
        for (auto const& input : job.memories)
            if (std::none_of(current->memories.begin(), current->memories.end(), [&input](Memory const& memory)
                { return memory.id == input.value.id && memory.contentRevision == input.value.contentRevision; }))
                return false;
        return true;
    }

    void RemoveJob(ActorKey owner)
    {
        jobs.erase(owner);
        std::erase(ready, owner);
    }

    bool ApplyTemplate(ActorKey owner, std::size_t count, FormationMode formation, uint64_t gameMs, uint64_t realMs)
    {
        auto const status = store.Status(owner);
        auto const* current = store.FindReady(owner);
        if (!status || !current || current->perceptions.empty())
            return false;
        count = std::min({count, current->perceptions.size(), BatchLimit});
        std::vector<Perception> inputs(current->perceptions.begin(), current->perceptions.begin() + count);
        auto groups = GroupInputs(inputs, policy, gameMs);
        std::vector<uint64_t> ids;
        std::vector<Memory> memories;
        for (auto& group : groups)
        {
            for (auto index : group.inputs)
                ids.push_back(inputs[index].id);
            group.memory.formation = formation;
            memories.push_back(std::move(group.memory));
        }
        auto const formed = memories.size();
        if (!store.Apply(owner, status->generation, ids, {}, std::move(memories), gameMs, realMs))
            return false;
        if (formation == FormationMode::Reflex)
            stats.reflexMemories += formed;
        else
            stats.fallbackMemories += formed;
        return true;
    }

    std::optional<Job> Capture(ActorKey owner)
    {
        auto const status = store.Status(owner);
        auto const* current = store.FindReady(owner);
        if (!status || !current || current->perceptions.empty())
            return std::nullopt;
        Job job;
        job.snapshot.bootEpoch = bootEpoch;
        job.snapshot.jobToken = RandomToken();
        job.snapshot.owner = owner;
        job.snapshot.actorGeneration = status->generation;
        job.snapshot.profileFingerprint = FakeProfile;
        job.snapshot.admittedGameTimeMs = current->perceptions.front().gameTimeMs;
        job.snapshot.admittedRealTimeMs = current->perceptions.front().admittedRealTimeMs;
        for (auto const& value : current->perceptions)
        {
            if (UsesReflexFormation(value) || job.snapshot.perceptions.size() >= BatchLimit)
                break;
            JobSnapshot candidate = job.snapshot;
            PerceptionInput input;
            input.token = "p" + std::to_string(candidate.perceptions.size() + 1);
            input.value = value;
            input.value.emissionId = 0; // Transport deduplication metadata is not character context.
            input.subjectToken = AddEntity(candidate, value.subject);
            input.sourceToken = AddEntity(candidate, value.source);
            input.placeToken = AddPlace(candidate, value.place);
            candidate.perceptions.push_back(std::move(input));
            auto const groups = GroupInputs(InputValues(candidate), policy, value.gameTimeMs);
            if (groups.empty() || groups.back().inputs.back() + 1 != candidate.perceptions.size())
                break;
            candidate.contextBytesBound = ContextBound(candidate);
            if (candidate.contextBytesBound > ContextLimit)
                break;
            job.snapshot = std::move(candidate);
        }
        if (job.snapshot.perceptions.empty())
            return std::nullopt;
        std::vector<Memory const*> strongest;
        for (auto const& memory : current->memories)
            strongest.push_back(&memory);
        std::sort(strongest.begin(), strongest.end(), [](Memory const* left, Memory const* right)
        {
            return left->salience > right->salience || (left->salience == right->salience && left->id < right->id);
        });
        for (auto const* memory : strongest)
        {
            if (job.snapshot.memories.size() >= MemoryLimit)
                break;
            JobSnapshot candidate = job.snapshot;
            candidate.memories.push_back({"m" + std::to_string(candidate.memories.size() + 1), *memory});
            AddEntity(candidate, memory->subject);
            AddEntity(candidate, memory->source);
            candidate.contextBytesBound = ContextBound(candidate);
            if (candidate.contextBytesBound <= ContextLimit)
                job.snapshot = std::move(candidate);
        }
        return job;
    }

    bool EnvelopeCurrent(ProposalEnvelope const& result, Job const& job,
        uint64_t gameMs, uint64_t realMs) const
    {
        auto const& input = job.snapshot;
        return job.leased && result.bootEpoch == bootEpoch && result.jobToken == input.jobToken
            && result.owner == input.owner && result.actorGeneration == input.actorGeneration
            && result.workerId == input.workerId && result.profileFingerprint == input.profileFingerprint
            && result.leaseGeneration == input.leaseGeneration && result.permitId == input.permitId
            && realMs < input.leaseExpiresRealTimeMs && realMs < input.permitExpiresRealTimeMs
            && !DeadlineExpired(input, gameMs, realMs) && PrefixCurrent(input) && MemoryContextCurrent(input);
    }

    enum class Acceptance
    {
        Applied,
        InvalidProposal,
        InvalidTarget
    };

    Acceptance Accept(ProposalEnvelope const& result, JobSnapshot const& job, uint64_t gameMs, uint64_t realMs)
    {
        if (result.memories.empty() || result.memories.size() > OutputLimit)
            return Acceptance::InvalidProposal;
        struct SupportedProposal
        {
            MemoryProposal const* proposal;
            std::vector<std::size_t> inputs;
        };
        std::set<std::size_t> accounted;
        std::vector<SupportedProposal> supported;
        for (auto const& proposal : result.memories)
        {
            if (proposal.operation > ProposalOperation::Supersede || proposal.kind > MemoryKind::Met
                || proposal.supportingPerceptions.empty() || proposal.supportingPerceptions.size() > BatchLimit
                || proposal.text.empty() || !IsBoundedText(proposal.text, 512) || !std::isfinite(proposal.confidence)
                || !std::isfinite(proposal.salience) || proposal.confidence < 0 || proposal.confidence > 1
                || proposal.salience < 0 || proposal.salience > 1)
                return Acceptance::InvalidProposal;
            SupportedProposal value{&proposal, {}};
            for (auto const& token : proposal.supportingPerceptions)
            {
                auto const input = std::find_if(job.perceptions.begin(), job.perceptions.end(),
                    [&token](PerceptionInput const& perception) { return perception.token == token; });
                if (input == job.perceptions.end())
                    return Acceptance::InvalidProposal;
                auto const index = std::size_t(input - job.perceptions.begin());
                if (!accounted.insert(index).second)
                    return Acceptance::InvalidProposal;
                value.inputs.push_back(index);
            }
            std::sort(value.inputs.begin(), value.inputs.end());
            supported.push_back(std::move(value));
        }
        if (accounted.size() != job.perceptions.size())
            return Acceptance::InvalidProposal;
        std::sort(supported.begin(), supported.end(), [](SupportedProposal const& left, SupportedProposal const& right)
            { return left.inputs.front() < right.inputs.front(); });
        std::vector<MemoryMutation> mutations;
        std::set<std::string> targetTokens;
        std::size_t next = 0;
        for (auto const& value : supported)
        {
            auto const& proposal = *value.proposal;
            auto const& first = job.perceptions[value.inputs.front()];
            auto memory = FormFallback(first.value, policy, gameMs);
            if (proposal.kind != memory.kind)
                return Acceptance::InvalidProposal;
            for (auto index : value.inputs)
            {
                if (index != next++)
                    return Acceptance::InvalidProposal; // Groups preserve an ordered contiguous input partition.
                auto const& input = job.perceptions[index];
                auto const derived = FormFallback(input.value, policy, gameMs);
                if (!Compatible(first.value, input.value, memory, derived)
                    || proposal.subjectToken != input.subjectToken || proposal.sourceToken != input.sourceToken
                    || proposal.placeToken != input.placeToken)
                    return Acceptance::InvalidProposal;
                memory.confidence = std::min(memory.confidence, derived.confidence);
            }
            memory.claim = proposal.text;
            memory.confidence = std::min(memory.confidence, proposal.confidence);
            memory.salience = std::min(proposal.salience, SalienceCeiling(memory));
            memory.formation = external ? FormationMode::Model : FormationMode::InProcessFake;
            MemoryMutation mutation;
            mutation.memory = std::move(memory);
            if (proposal.operation == ProposalOperation::Create)
            {
                if (!proposal.targetMemoryToken.empty())
                    return Acceptance::InvalidTarget;
            }
            else
            {
                auto const target = std::find_if(job.memories.begin(), job.memories.end(),
                    [&proposal](MemoryInput const& input) { return input.token == proposal.targetMemoryToken; });
                if (target == job.memories.end() || !targetTokens.insert(target->token).second)
                    return Acceptance::InvalidTarget;
                mutation.target = MemoryTarget{job.owner, {target->value.id, target->value.contentRevision}};
                mutation.kind = proposal.operation == ProposalOperation::Reinforce
                    ? MemoryMutationKind::Reinforce : MemoryMutationKind::Supersede;
            }
            mutations.push_back(std::move(mutation));
        }
        std::vector<uint64_t> ids;
        for (auto const& input : job.perceptions)
            ids.push_back(input.value.id);
        std::vector<MemoryVersion> expected;
        for (auto const& input : job.memories)
            expected.push_back({input.value.id, input.value.contentRevision});
        if (!store.ApplyMutations(job.owner, job.actorGeneration, ids, expected, mutations, gameMs, realMs))
            return Acceptance::InvalidTarget;
        for (auto const& mutation : mutations)
        {
            if (external)
                ++stats.modelMemories;
            else if (mutation.kind == MemoryMutationKind::Create)
                ++stats.fakeMemories;
            else if (mutation.kind == MemoryMutationKind::Reinforce)
                ++stats.fakeReinforcements;
            else
                ++stats.fakeSupersessions;
        }
        return Acceptance::Applied;
    }

    void Dispatch(uint64_t realMs, std::size_t& budget)
    {
        auto leased = std::count_if(jobs.begin(), jobs.end(), [](auto const& item) { return item.second.leased; });
        std::size_t candidates = ready.size();
        while (budget && candidates-- && std::size_t(leased) < limits.concurrentLeases && !ready.empty())
        {
            auto const owner = ready.front();
            ready.pop_front();
            auto iterator = jobs.find(owner);
            if (iterator == jobs.end())
                continue;
            auto& job = iterator->second;
            auto& schedule = owners.at(owner);
            if (realMs < schedule.nextTokenRealMs)
            {
                ready.push_back(owner);
                continue;
            }
            auto& input = job.snapshot;
            input.workerId = FakeWorkerId;
            input.leaseGeneration = ++nextLeaseGeneration;
            input.permitId = "fake:" + RandomToken();
            input.leaseExpiresRealTimeMs = std::min(AddTime(realMs, LeaseMs),
                AddTime(input.admittedRealTimeMs, RealDeadlineMs));
            input.permitExpiresRealTimeMs = std::min(AddTime(realMs, FakePermitMs),
                AddTime(input.admittedRealTimeMs, RealDeadlineMs));
            job.leased = true;
            job.behavior = fakeBehavior;
            job.nextHeartbeatRealMs = AddTime(realMs, HeartbeatMs);
            job.resultDueRealMs = AddTime(realMs, fakeBehavior.delayMs);
            if (fakeBehavior.mode != FakeMode::Withhold)
            {
                job.fakeResult = MakeFakeProposal(input, policy);
                if (fakeBehavior.mode == FakeMode::Malformed)
                    job.fakeResult->memories.front().supportingPerceptions = {"unknown-perception"};
                else if (fakeBehavior.mode == FakeMode::Stale)
                    job.fakeResult->permitId = "fake:expired";
            }
            schedule.nextTokenRealMs = AddTime(realMs, OwnerTokenIntervalMs);
            ++stats.dispatched;
            ++leased;
            --budget;
        }
    }

    ActorStore& store;
    MemoryPolicy policy;
    CoordinatorLimits limits;
    std::string bootEpoch;
    std::thread::id thread = std::this_thread::get_id();
    std::map<ActorKey, Owner> owners;
    std::map<ActorKey, Job> jobs;
    std::deque<ActorKey> ready;
    std::deque<ProposalEnvelope> results;
    FakeBehavior fakeBehavior;
    CoordinatorStats stats;
    uint64_t nextLeaseGeneration = 0;
    bool stopping = false;
    bool external = false;
};

PilotCoordinator::PilotCoordinator(ActorStore& store, MemoryPolicy policy, CoordinatorLimits limits)
    : _impl(std::make_unique<Impl>(store, policy, limits))
{
    if (!IsValidPolicy(policy) || !limits.owners || !limits.jobs || !limits.resultQueue || !limits.concurrentLeases
        || limits.concurrentLeases > limits.jobs || limits.owners > 256
        || limits.jobs > 256 || limits.resultQueue > 256)
        ABORT("Invalid Alles pilot coordinator limits or policy");
}

PilotCoordinator::~PilotCoordinator() = default;

bool PilotCoordinator::Track(ActorKey owner)
{
    _impl->CheckThread();
    if (_impl->stopping || !IsValidActor(owner))
        return false;
    if (_impl->owners.contains(owner))
        return true;
    if (_impl->owners.size() >= _impl->limits.owners)
        return false;
    _impl->owners.emplace(owner, Impl::Owner{});
    return true;
}

bool PilotCoordinator::Forget(ActorKey owner)
{
    _impl->CheckThread();
    if (_impl->jobs.contains(owner))
        return false;
    auto const status = _impl->store.Status(owner);
    if (status && status->state == ActorState::Loading)
        return false; // Loading may own buffered observations that FindReady cannot expose yet.
    auto const* current = _impl->store.FindReady(owner);
    if (current && !current->perceptions.empty())
        return false;
    return _impl->owners.erase(owner) != 0;
}

bool PilotCoordinator::Submit(ProposalEnvelope result)
{
    _impl->CheckThread();
    if (_impl->stopping || _impl->results.size() >= _impl->limits.resultQueue || result.memories.size() > 4)
    {
        ++_impl->stats.resultDrops;
        return false;
    }
    // W4b must enforce its frame bound before decoding; this value boundary also bounds every retained string.
    if (result.bootEpoch.size() > 64 || result.jobToken.size() > 64 || result.workerId.size() > 64
        || result.profileFingerprint.size() > 128 || result.permitId.size() > 64)
        return false;
    for (auto const& memory : result.memories)
    {
        if (memory.supportingPerceptions.size() > BatchLimit || memory.text.size() > 2048
            || memory.targetMemoryToken.size() > 64 || memory.subjectToken.size() > 64
            || memory.sourceToken.size() > 64 || memory.placeToken.size() > 64)
            return false;
        for (auto const& token : memory.supportingPerceptions)
            if (token.size() > 64)
                return false;
    }
    _impl->results.push_back(std::move(result));
    return true;
}

void PilotCoordinator::SetFakeBehavior(FakeBehavior behavior)
{
    _impl->CheckThread();
    _impl->fakeBehavior = behavior;
}

std::optional<JobSnapshot> PilotCoordinator::Inspect(ActorKey owner) const
{
    _impl->CheckThread();
    auto const iterator = _impl->jobs.find(owner);
    if (iterator == _impl->jobs.end())
        return std::nullopt;
    return iterator->second.snapshot;
}

CoordinatorStats const& PilotCoordinator::Stats() const
{
    _impl->CheckThread();
    return _impl->stats;
}

void PilotCoordinator::Update(uint64_t gameTimeMs, uint64_t realTimeMs, std::size_t itemBudget)
{
    _impl->CheckThread();
    if (_impl->stopping || !itemBudget)
        return;

    // Expiry is tested at processing time before any previously queued result can apply.
    for (auto iterator = _impl->jobs.begin(); iterator != _impl->jobs.end() && itemBudget;)
    {
        auto const owner = iterator->first;
        auto const& job = iterator->second;
        bool const expired = DeadlineExpired(job.snapshot, gameTimeMs, realTimeMs)
            || (job.leased && (realTimeMs >= job.snapshot.leaseExpiresRealTimeMs
                || (!job.snapshot.permitId.empty() && realTimeMs >= job.snapshot.permitExpiresRealTimeMs)));
        if (!_impl->PrefixCurrent(job.snapshot) || (!expired && !_impl->MemoryContextCurrent(job.snapshot)))
        {
            // Known-invalid context must release capacity now. A replacement captures current memories,
            // but its retained input admission times and already charged owner bucket do not reset.
            ++_impl->stats.invalidatedJobs;
            ++iterator;
            _impl->RemoveJob(owner);
            --itemBudget;
            continue;
        }
        if (expired && _impl->ApplyTemplate(owner, job.snapshot.perceptions.size(),
            FormationMode::Fallback, gameTimeMs, realTimeMs))
        {
            ++_impl->stats.expiredJobs;
            if (Elapsed(gameTimeMs, job.snapshot.admittedGameTimeMs, GameDeadlineMs))
                ++_impl->stats.gameDeadlineExpiries;
            else if (Elapsed(realTimeMs, job.snapshot.admittedRealTimeMs, RealDeadlineMs))
                ++_impl->stats.realDeadlineExpiries;
            else
                ++_impl->stats.leaseExpiries;
            ++iterator;
            _impl->RemoveJob(owner);
            --itemBudget;
        }
        else
            ++iterator;
    }

    while (itemBudget && !_impl->results.empty())
    {
        auto result = std::move(_impl->results.front());
        _impl->results.pop_front();
        --itemBudget;
        auto const iterator = _impl->jobs.find(result.owner);
        if (iterator == _impl->jobs.end()
            || !_impl->EnvelopeCurrent(result, iterator->second, gameTimeMs, realTimeMs))
        {
            ++_impl->stats.staleResults;
            continue;
        }
        auto const owner = result.owner;
        auto const acceptance = _impl->Accept(result, iterator->second.snapshot, gameTimeMs, realTimeMs);
        if (acceptance == Impl::Acceptance::Applied)
            _impl->RemoveJob(owner);
        else
        {
            ++_impl->stats.invalidResults;
            if (acceptance == Impl::Acceptance::InvalidTarget)
            {
                // Invalid targets cannot authorize even a partial semantic transition. Retain inputs,
                // fence this job and let their original admission deadlines/bucket govern a fresh snapshot.
                _impl->RemoveJob(owner);
            }
            else if (_impl->ApplyTemplate(owner, iterator->second.snapshot.perceptions.size(),
                FormationMode::Fallback, gameTimeMs, realTimeMs))
                _impl->RemoveJob(owner);
        }
    }

    std::vector<ActorKey> candidates;
    for (auto const& [owner, unused] : _impl->owners)
    {
        if (_impl->jobs.contains(owner))
            continue;
        auto const* current = _impl->store.FindReady(owner);
        if (current && !current->perceptions.empty())
            candidates.push_back(owner);
    }
    std::sort(candidates.begin(), candidates.end(), [this](ActorKey left, ActorKey right)
    {
        auto const& first = _impl->store.FindReady(left)->perceptions.front();
        auto const& second = _impl->store.FindReady(right)->perceptions.front();
        if (first.gameTimeMs != second.gameTimeMs)
            return first.gameTimeMs < second.gameTimeMs;
        if (first.admittedRealTimeMs != second.admittedRealTimeMs)
            return first.admittedRealTimeMs < second.admittedRealTimeMs;
        return left < right;
    });
    for (auto const owner : candidates)
    {
        if (!itemBudget)
            break;
        auto const& first = _impl->store.FindReady(owner)->perceptions.front();
        if (UsesReflexFormation(first))
        {
            if (_impl->ApplyTemplate(owner, 1, FormationMode::Reflex, gameTimeMs, realTimeMs))
                --itemBudget;
        }
        else if (Elapsed(realTimeMs, first.admittedRealTimeMs, RealDeadlineMs)
            || Elapsed(gameTimeMs, first.gameTimeMs, GameDeadlineMs))
        {
            // This path also drains owners whose work waited outside the bounded job queue.
            bool const gameExpired = Elapsed(gameTimeMs, first.gameTimeMs, GameDeadlineMs);
            if (_impl->ApplyTemplate(owner, 1, FormationMode::Fallback, gameTimeMs, realTimeMs))
            {
                ++_impl->stats.expiredJobs;
                if (gameExpired)
                    ++_impl->stats.gameDeadlineExpiries;
                else
                    ++_impl->stats.realDeadlineExpiries;
                --itemBudget;
            }
        }
        else if (Elapsed(gameTimeMs, first.gameTimeMs, DebounceMs) && _impl->jobs.size() < _impl->limits.jobs)
        {
            if (auto job = _impl->Capture(owner))
            {
                _impl->jobs.emplace(owner, std::move(*job));
                _impl->ready.push_back(owner);
            }
            else
            {
                ++_impl->stats.contextOverflows;
                _impl->ApplyTemplate(owner, 1, FormationMode::Fallback, gameTimeMs, realTimeMs);
            }
            --itemBudget;
        }
    }
    if (_impl->external)
        return;
    _impl->Dispatch(realTimeMs, itemBudget);

    for (auto& [owner, job] : _impl->jobs)
    {
        if (!job.leased)
            continue;
        if (job.behavior.heartbeats && realTimeMs >= job.nextHeartbeatRealMs
            && realTimeMs < job.snapshot.leaseExpiresRealTimeMs)
        {
            job.snapshot.leaseExpiresRealTimeMs = std::min(AddTime(realTimeMs, LeaseMs),
                AddTime(job.snapshot.admittedRealTimeMs, RealDeadlineMs));
            job.nextHeartbeatRealMs = AddTime(realTimeMs, HeartbeatMs);
        }
        if (job.fakeResult && realTimeMs >= job.resultDueRealMs)
        {
            Submit(std::move(*job.fakeResult));
            job.fakeResult.reset();
        }
    }
}

void PilotCoordinator::Stop()
{
    _impl->CheckThread();
    _impl->stopping = true;
    _impl->jobs.clear();
    _impl->ready.clear();
    _impl->results.clear();
}

void PilotCoordinator::EnableExternal()
{
    _impl->CheckThread();
    ASSERT(_impl->jobs.empty());
    _impl->external = true;
}

std::optional<JobSnapshot> PilotCoordinator::Claim(std::string worker, std::string profile,
    uint64_t gameMs, uint64_t realMs, std::optional<ActorKey> selected)
{
    _impl->CheckThread();
    if (!_impl->external || _impl->stopping || worker.empty() || worker.size() > 64 || profile.size() > 128
        || (!selected && std::any_of(_impl->jobs.begin(), _impl->jobs.end(),
            [](auto const& pair) { return pair.second.leased; })))
        return std::nullopt;
    auto candidates = _impl->ready.size();
    while (candidates-- && !_impl->ready.empty())
    {
        auto const owner = _impl->ready.front();
        _impl->ready.pop_front();
        auto found = _impl->jobs.find(owner);
        if (found == _impl->jobs.end())
            continue;
        auto& job = found->second;
        auto& input = job.snapshot;
        auto& schedule = _impl->owners.at(owner);
        if ((selected && owner != *selected) || job.leased ||
            realMs < schedule.nextTokenRealMs || DeadlineExpired(input, gameMs, realMs)
            || AddTime(realMs, 25000) > AddTime(input.admittedRealTimeMs, RealDeadlineMs))
        {
            _impl->ready.push_back(owner);
            continue;
        }
        input.workerId = std::move(worker);
        input.profileFingerprint = std::move(profile);
        input.leaseGeneration = ++_impl->nextLeaseGeneration;
        input.leaseExpiresRealTimeMs = std::min(AddTime(realMs, LeaseMs),
            AddTime(input.admittedRealTimeMs, RealDeadlineMs));
        job.leased = true;
        schedule.nextTokenRealMs = realMs; // External scheduling/rate policy owns admission; no hidden pilot bucket.
        ++_impl->stats.dispatched;
        return input;
    }
    return std::nullopt;
}

std::vector<JobSnapshot> PilotCoordinator::Ready(uint64_t gameMs, uint64_t realMs) const
{
    _impl->CheckThread();
    std::vector<JobSnapshot> result;
    for (auto owner : _impl->ready)
    {
        auto found = _impl->jobs.find(owner);
        if (found == _impl->jobs.end() || found->second.leased)
            continue;
        auto const& job = found->second.snapshot;
        if (realMs >= _impl->owners.at(owner).nextTokenRealMs && !DeadlineExpired(job, gameMs, realMs) &&
            AddTime(realMs, 25000) <= AddTime(job.admittedRealTimeMs, RealDeadlineMs))
            result.push_back(job);
    }
    return result;
}

void PilotCoordinator::RejectQueued(std::string const& token, uint64_t gameMs, uint64_t realMs)
{
    _impl->CheckThread();
    auto found = std::find_if(_impl->jobs.begin(), _impl->jobs.end(), [&](auto const& pair)
        { return pair.second.snapshot.jobToken == token && !pair.second.leased; });
    if (found == _impl->jobs.end())
        return;
    auto const owner = found->first;
    auto const& job = found->second.snapshot;
    if (_impl->PrefixCurrent(job))
        _impl->ApplyTemplate(owner, job.perceptions.size(), FormationMode::Fallback, gameMs, realMs);
    // If persistence/handoff prevents the fallback, retained perceptions keep their original bounded age.
    _impl->RemoveJob(owner);
}

bool PilotCoordinator::Heartbeat(std::string const& token, std::string const& worker,
    uint64_t lease, uint64_t realMs)
{
    _impl->CheckThread();
    for (auto& [owner, job] : _impl->jobs)
    {
        auto& input = job.snapshot;
        if (job.leased && input.jobToken == token && input.workerId == worker && input.leaseGeneration == lease
            && realMs < input.leaseExpiresRealTimeMs && realMs < AddTime(input.admittedRealTimeMs, RealDeadlineMs))
        {
            input.leaseExpiresRealTimeMs = std::min(AddTime(realMs, LeaseMs),
                AddTime(input.admittedRealTimeMs, RealDeadlineMs));
            return true;
        }
    }
    return false;
}

bool PilotCoordinator::Authorize(std::string const& token, std::string const& worker, uint64_t lease,
    std::string permit, uint64_t gameMs, uint64_t realMs)
{
    _impl->CheckThread();
    for (auto& [owner, job] : _impl->jobs)
    {
        auto& input = job.snapshot;
        if (job.leased && input.jobToken == token && input.workerId == worker && input.leaseGeneration == lease
            && input.permitId.empty() && realMs < input.leaseExpiresRealTimeMs
            && !DeadlineExpired(input, gameMs, realMs) && _impl->PrefixCurrent(input)
            && _impl->MemoryContextCurrent(input)
            && AddTime(realMs, 25000) <= AddTime(input.admittedRealTimeMs, RealDeadlineMs))
        {
            input.permitId = std::move(permit);
            input.permitExpiresRealTimeMs = AddTime(realMs, 25000);
            input.httpAttemptCount = 1;
            return true;
        }
    }
    return false;
}

bool PilotCoordinator::Release(std::string const& token, std::string const& worker, uint64_t lease)
{
    _impl->CheckThread();
    for (auto& [owner, job] : _impl->jobs)
        if (job.leased && job.snapshot.jobToken == token && job.snapshot.workerId == worker
            && job.snapshot.leaseGeneration == lease && job.snapshot.permitId.empty())
        {
            job.leased = false;
            job.snapshot.workerId.clear();
            _impl->ready.push_back(owner);
            return true;
        }
    return false;
}

std::string PilotCoordinator::ApplyExternal(ProposalEnvelope const& result, uint64_t gameMs, uint64_t realMs)
{
    _impl->CheckThread();
    auto const job = _impl->jobs.find(result.owner);
    if (!_impl->external || _impl->stopping || job == _impl->jobs.end()
        || !_impl->EnvelopeCurrent(result, job->second, gameMs, realMs))
    {
        ++_impl->stats.staleResults;
        return "stale";
    }
    auto const outcome = _impl->Accept(result, job->second.snapshot, gameMs, realMs);
    if (outcome == Impl::Acceptance::Applied)
    {
        _impl->RemoveJob(result.owner);
        return "applied";
    }
    ++_impl->stats.invalidResults;
    return "invalid";
}

std::size_t PilotCoordinator::DrainFallback(uint64_t gameTimeMs, uint64_t realTimeMs, std::size_t itemBudget)
{
    _impl->CheckThread();
    if (!_impl->stopping)
        return 0;
    std::size_t applied = 0;
    for (auto const& [owner, unused] : _impl->owners)
    {
        while (applied < itemBudget)
        {
            auto const* current = _impl->store.FindReady(owner);
            if (!current || current->perceptions.empty())
                break;
            // Preserve planned Reflex accounting even in a village shutdown drain.
            bool const reflex = UsesReflexFormation(current->perceptions.front());
            std::size_t count = 0;
            for (auto const& perception : current->perceptions)
            {
                if (count >= BatchLimit || UsesReflexFormation(perception) != reflex)
                    break;
                ++count;
            }
            if (!_impl->ApplyTemplate(owner, count, reflex ? FormationMode::Reflex : FormationMode::Fallback,
                gameTimeMs, realTimeMs))
                break;
            ++applied;
        }
    }
    return applied;
}
}
