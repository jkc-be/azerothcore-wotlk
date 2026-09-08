/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "Cooperation.h"
#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace Alles
{
namespace
{
bool PlayerActor(ActorKey actor)
{
    return IsValidActor(actor) && actor.kind == ActorKind::Player;
}

bool ValidAgreement(CompanionAgreement const& agreement)
{
    return agreement.person.actor && PlayerActor(*agreement.person.actor)
        && !agreement.person.name.empty() && IsBoundedText(agreement.person.name, 100)
        && !agreement.statement.empty() && IsBoundedText(agreement.statement, 512);
}

bool Startable(Cooperation const& state, uint64_t now)
{
    return state.state == CooperationState::None || (state.state == CooperationState::Deferred
        && state.attempts < 2 && now >= state.reconsiderMs);
}

void Defer(Cooperation& state, std::string reason, uint64_t now)
{
    state.state = CooperationState::Deferred;
    state.reason = std::move(reason);
    state.reconsiderMs = now + 600000;
}
}

bool IsValidCooperation(Cooperation const& state)
{
    if (state.state == CooperationState::None)
        return state == Cooperation{};
    if (state.state > CooperationState::Cancelled || !PlayerActor(state.owner) || !PlayerActor(state.leader)
        || !state.objective || !state.quest || !state.rendezvousPlace || !state.attempts || state.attempts > 2
        || state.deadlineMs <= state.startedMs || state.deadlineMs - state.startedMs > 1800000
        || !IsBoundedText(state.reason, 512) || state.agreements.empty() || state.agreements.size() > 5
        || !state.agreements.contains(state.owner) || !state.agreements.contains(state.leader))
        return false;
    for (auto const& [actor, agreement] : state.agreements)
        if (!ValidAgreement(agreement) || actor != *agreement.person.actor
            || agreement.agreedMs < state.startedMs || agreement.agreedMs > state.deadlineMs
            || (agreement.reportedBy && (agreement.reportedBy->actor != state.leader
                || agreement.reportedBy->name.empty() || !IsBoundedText(agreement.reportedBy->name, 100)
                || actor == state.owner || actor == state.leader)))
            return false;
    return true;
}

bool BeginRecruitment(Cooperation& state, ActorKey owner, uint64_t objective, uint32_t quest,
    uint32_t place, CompanionAgreement self, uint64_t now)
{
    if (!IsValidCooperation(state) || !Startable(state, now)
        || (state.state != CooperationState::None
            && (state.owner != owner || state.objective != objective || state.quest != quest))
        || !ValidAgreement(self) || self.reportedBy || *self.person.actor != owner
        || !PlayerActor(owner) || !objective || !quest || !place
        || now > std::numeric_limits<uint64_t>::max() - 1800000)
        return false;
    auto const attempts = state.attempts + 1;
    self.agreedMs = now;
    state = {CooperationState::Recruiting, owner, owner, objective, quest, place, attempts, now, now + 120000, 0,
        "Seeking willing companions for this quest", {{owner, std::move(self)}}};
    return true;
}

bool AcceptCooperation(Cooperation& state, ActorKey owner, uint64_t objective, uint32_t quest,
    uint32_t place, CompanionAgreement self, CompanionAgreement leader, uint64_t now)
{
    if (!ValidAgreement(leader) || leader.reportedBy || *leader.person.actor == owner)
        return false;
    auto staged = state;
    if (!BeginRecruitment(staged, owner, objective, quest, place, std::move(self), now))
        return false;
    auto const actor = *leader.person.actor;
    leader.agreedMs = now;
    staged.leader = actor;
    staged.agreements.emplace(actor, std::move(leader));
    staged.state = CooperationState::Agreed;
    staged.deadlineMs = now + 300000;
    staged.reason = "Agreed to help; normal invitation and actual party membership are still required";
    state = std::move(staged);
    return true;
}

bool AgreeCompanion(Cooperation& state, CompanionAgreement agreement, uint64_t now)
{
    if ((state.state != CooperationState::Recruiting && state.state != CooperationState::Agreed
        && state.state != CooperationState::Rendezvous)
        || !ValidAgreement(agreement) || agreement.reportedBy || now < state.startedMs || now >= state.deadlineMs
        || state.agreements.contains(*agreement.person.actor) || state.agreements.size() >= 5)
        return false;
    auto const actor = *agreement.person.actor;
    agreement.agreedMs = now;
    state.agreements.emplace(actor, std::move(agreement));
    if (state.state == CooperationState::Recruiting)
        state.deadlineMs = state.startedMs + 300000;
    state.state = CooperationState::Agreed;
    state.reason = "A companion agreed; invitation and membership still need verification";
    return true;
}

bool AdoptCooperativeRoster(Cooperation& state, Reference const& source, std::vector<Reference> const& members,
    std::string const& statement, uint64_t now)
{
    if (!IsValidCooperation(state) || state.owner == state.leader || source.actor != state.leader
        || source.name.empty() || !IsBoundedText(source.name, 100) || statement.empty()
        || !IsBoundedText(statement, 255) || members.size() < 2 || members.size() > 5
        || now < state.startedMs || now >= state.deadlineMs
        || (state.state != CooperationState::Agreed && state.state != CooperationState::Rendezvous
            && state.state != CooperationState::Working))
        return false;
    std::map<ActorKey, Reference> roster;
    for (auto const& member : members)
        if (!member.actor || !PlayerActor(*member.actor) || member.name.empty() || !IsBoundedText(member.name, 100)
            || statement.find(member.name) == std::string::npos || !roster.emplace(*member.actor, member).second)
            return false;
    if (!roster.contains(state.owner) || !roster.contains(state.leader))
        return false;
    for (auto const& [actor, agreement] : state.agreements)
        if (!roster.contains(actor) || roster.at(actor).name != agreement.person.name)
            return false;
    if (state.state == CooperationState::Working && roster.size() != state.agreements.size())
        return false;
    auto staged = state;
    for (auto const& [actor, person] : roster)
        if (!staged.agreements.contains(actor))
            staged.agreements.emplace(actor, CompanionAgreement{person, statement, now, source});
    if (!IsValidCooperation(staged))
        return false;
    state = std::move(staged);
    return true;
}

bool ObserveCooperation(Cooperation& state, PartyObservation const& observed, uint64_t now)
{
    if (state.state == CooperationState::None || state.state == CooperationState::Completed
        || state.state == CooperationState::Cancelled
        || state.state == CooperationState::Deferred || now < state.startedMs
        || now > std::numeric_limits<uint64_t>::max() - 1800000)
        return false;
    auto const before = state;
    if (observed.ownerControlled)
    {
        state.state = CooperationState::Cancelled;
        state.reason = "Human control superseded the cooperative intention";
    }
    else if (state.state != CooperationState::Completed && now >= state.deadlineMs)
        Defer(state, "The bounded recruitment or rendezvous/work window expired", now);
    else if (observed.group && (!observed.ordinaryParty || observed.leader != state.leader
        || observed.members.size() > 5 || observed.requiredMembers < 2 || observed.requiredMembers > 5))
        Defer(state, "The observed group does not match the agreed ordinary party", now);
    else if (observed.group)
    {
        std::set<ActorKey> members;
        bool ready = true;
        bool finished = true;
        bool compatible = true;
        for (auto const& member : observed.members)
        {
            compatible = compatible && members.insert(member.actor).second && state.agreements.contains(member.actor);
            ready = ready && member.online && member.alive && member.inRendezvous && member.eligible && member.ready;
            finished = finished && member.online && member.finished;
        }
        compatible = compatible && members.contains(state.owner) && members.contains(state.leader);
        bool const completeMembership = members.size() >= observed.requiredMembers
            && members.size() == state.agreements.size();
        if (!compatible)
            Defer(state, "Observed membership includes an unagreed companion or lacks the owner/leader", now);
        else if (!completeMembership && (state.state == CooperationState::Working
            || state.state == CooperationState::Completed))
            Defer(state, "An agreed companion departed before the collective activity was reconciled", now);
        else if (completeMembership && finished)
        {
            state.state = CooperationState::Completed;
            state.reason = "Every agreed participant's own completion was observed";
        }
        else if (completeMembership && ready)
        {
            if (state.state != CooperationState::Working)
                state.deadlineMs = state.startedMs + 1800000;
            state.state = CooperationState::Working;
            state.reason = "The agreed party is present, eligible and ready; each owner tracks individual progress";
        }
        else if (state.state == CooperationState::Working)
        {
            if (std::any_of(observed.members.begin(), observed.members.end(),
                [](auto const& member) { return !member.online || !member.eligible; }))
                Defer(state, "An agreed participant is unavailable or no longer eligible", now);
            // Recovery and catching up pause execution, without forgetting the established rendezvous.
        }
        else
        {
            if (state.state == CooperationState::Agreed || state.state == CooperationState::Recruiting)
                state.deadlineMs = std::min(state.startedMs + 1800000, now + 300000);
            state.state = CooperationState::Rendezvous;
            state.reason = "Actual membership, arrival and readiness are still being established";
        }
    }
    else if (state.state == CooperationState::Working || state.state == CooperationState::Rendezvous
        || state.state == CooperationState::Completed)
        Defer(state, "The previously observed party is absent", now);
    return state != before;
}

bool DeferCooperation(Cooperation& state, std::string reason, uint64_t now)
{
    if (!IsValidCooperation(state) || state.state == CooperationState::None
        || state.state >= CooperationState::Completed || now < state.startedMs
        || now > std::numeric_limits<uint64_t>::max() - 600000 || reason.empty() || !IsBoundedText(reason, 512))
        return false;
    Defer(state, std::move(reason), now);
    return true;
}

std::optional<ActorKey> CooperativeGuide(Cooperation const& state, PartyObservation const& observed)
{
    if (state.state != CooperationState::Working || !observed.ordinaryParty || !observed.group
        || observed.ownerControlled || observed.leader != state.leader
        || observed.members.size() != state.agreements.size())
        return std::nullopt;
    CompanionObservation const* choice = nullptr;
    for (auto const& member : observed.members)
    {
        if (!state.agreements.contains(member.actor) || !member.online || !member.eligible)
            return std::nullopt;
        if (member.finished)
            continue;
        auto rank = [&](CompanionObservation const& value)
        {
            return std::tuple{value.readyToReward, value.actor != state.leader, value.actor};
        };
        if (!choice || rank(member) < rank(*choice))
            choice = &member;
    }
    return choice ? std::optional<ActorKey>(choice->actor) : std::nullopt;
}

void ReconcileCooperation(Cooperation& state, uint64_t now)
{
    if (state.state == CooperationState::Working || state.state == CooperationState::Rendezvous
        || state.state == CooperationState::Completed)
    {
        if (now >= state.deadlineMs)
        {
            if (now <= std::numeric_limits<uint64_t>::max() - 600000)
                Defer(state, "Recheck the retained agreement after the expired saved activity", now);
        }
        else
        {
            state.state = CooperationState::Agreed;
            state.reason = "Reconcile real membership and each participant's progress after reload";
        }
    }
}

char const* Name(CooperationState state)
{
    switch (state)
    {
        case CooperationState::None: return "none";
        case CooperationState::Recruiting: return "recruiting";
        case CooperationState::Agreed: return "agreed";
        case CooperationState::Rendezvous: return "rendezvous";
        case CooperationState::Working: return "working";
        case CooperationState::Completed: return "completed";
        case CooperationState::Deferred: return "deferred";
        case CooperationState::Cancelled: return "cancelled";
    }
    return "invalid";
}
}
