/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Objective.h"
#include <tuple>
#include <algorithm>
#include <limits>
#include <set>

namespace Alles
{
namespace
{
bool Terminal(ObjectiveState state)
{
    return state == ObjectiveState::Completed || state == ObjectiveState::Cancelled;
}
}

Obstruction QuestReadinessObstruction(QuestReadiness const& readiness)
{
    if (readiness.rewarded)
        return Obstruction::None;
    if (readiness.failed)
        return Obstruction::Prerequisite;
    if (readiness.turningIn)
        return readiness.insufficientMoney ? Obstruction::Supplies : Obstruction::None;
    if (readiness.unsupportedExecutor)
        return Obstruction::Executor;
    if (readiness.criticallyDamagedEquipment)
        return Obstruction::Supplies;
    if (readiness.aboveLevelCapability)
        return Obstruction::Strength;
    if (readiness.needsCompanions)
        return Obstruction::Companions;
    return Obstruction::None;
}

QuestOpportunity ClassifyQuestOpportunity(uint32_t available, uint32_t tagged, uint32_t corpses, bool completeView)
{
    if (available)
        return QuestOpportunity::Available;
    if (!completeView)
        return QuestOpportunity::Unknown;
    if (tagged)
        return QuestOpportunity::Tagged;
    return corpses ? QuestOpportunity::Respawn : QuestOpportunity::Unknown;
}

bool IsValidObjectiveSnapshot(ObjectiveSnapshot const& snapshot, ObjectivePolicy const& policy)
{
    if (!snapshot.nextId || snapshot.nextId == std::numeric_limits<uint64_t>::max()
        || snapshot.objectives.size() > policy.maxObjectives)
        return false;
    std::set<uint32_t> unfinishedQuests;
    std::set<std::tuple<uint32_t, PlacePurpose, std::optional<ActorKey>>> unfinishedPlaces;
    unsigned current = 0;
    unsigned cooperating = 0;
    unsigned following = 0;
    unsigned preparing = 0;
    for (auto const& [id, objective] : snapshot.objectives)
    {
        if (objective.purpose > PlacePurpose::Companionship || objective.activityMs > 60000
            || (objective.purpose == PlacePurpose::Work && (objective.activityMs || objective.completedMs))
            || (objective.purpose != PlacePurpose::Work && (objective.quest || objective.request
                || !objective.place || objective.approach != ActivityCapability(objective.purpose)
                || objective.preparation || objective.cooperation.state != CooperationState::None
                || objective.checkpoint != QuestProgress{} || objective.discoveredQuest
                || objective.information != InformationSearch{}
                || (objective.purpose != PlacePurpose::Rest && objective.activityMs)
                || (objective.completedMs && (!objective.arrivedMs || objective.completedMs < objective.arrivedMs
                    || (objective.purpose == PlacePurpose::Rest && objective.activityMs != 60000)))
                || bool(objective.person) != (objective.purpose == PlacePurpose::Companionship)
                || (objective.person && objective.person->kind != ActorKind::Player)
                || (objective.state == ObjectiveState::Completed) != bool(objective.completedMs))))
            return false;
        if (objective.preparation)
        {
            auto const& preparation = *objective.preparation;
            if (!objective.quest || objective.request || preparation.state > PreparationState::Cancelled
                || preparation.kind > PreparationKind::BuyQuestSupplies
                || (preparation.kind == PreparationKind::RepairEquipment && (preparation.item || preparation.count))
                || (preparation.kind == PreparationKind::BuyQuestSupplies && (!preparation.item || !preparation.count))
                || preparation.attempts >= std::numeric_limits<uint32_t>::max() - 1
                || preparation.attemptsInCircumstances > 3 || preparation.attemptsInCircumstances > preparation.attempts
                || preparation.transactions > preparation.attempts
                || (!preparation.fundsKnown && (preparation.ownMoney || preparation.fundsAtAttempt))
                || preparation.spentMoney > uint64_t(preparation.transactions) * std::numeric_limits<uint32_t>::max()
                || preparation.earnedMoney > uint64_t(preparation.transactions) * std::numeric_limits<uint32_t>::max()
                || !IsBoundedText(preparation.reason, 512) || preparation.reason.empty()
                || (preparation.attempts && (!preparation.startedMs
                    || preparation.deadlineMs <= preparation.startedMs
                    || preparation.deadlineMs - preparation.startedMs != 120000))
                || (!preparation.attempts && (preparation.startedMs || preparation.deadlineMs
                    || preparation.transactions || preparation.spentMoney || preparation.lastTransactionMs))
                || bool(preparation.lastTransactionMs) != bool(preparation.transactions)
                || preparation.lastTransactionMs > preparation.deadlineMs
                || (preparation.state == PreparationState::Active && (!preparation.attempts
                    || objective.state != ObjectiveState::Deferred || ++preparing > 1)))
                return false;
        }
        if (objective.cooperation.state > CooperationState::None
            && objective.cooperation.state < CooperationState::Completed && ++cooperating > 1)
            return false;
        if (!IsValidCooperation(objective.cooperation)
            || (objective.cooperation.state != CooperationState::None
                && (objective.cooperation.objective != id || objective.cooperation.quest != objective.quest)))
            return false;
        if (objective.request)
        {
            auto const& request = *objective.request;
            if (objective.quest || objective.place || objective.person != request.source.actor
                || !request.source.actor || request.source.actor->kind != ActorKind::Player
                || request.source.name.empty() || !IsBoundedText(request.source.name, 100)
                || request.statement.empty() || !IsBoundedText(request.statement, 255)
                || !IsBoundedText(request.targetName, 100) || request.action > RequestAction::Assist
                || (request.action == RequestAction::Assist && request.targetName.empty())
                || (request.action != RequestAction::Assist && !request.targetName.empty())
                || objective.approach != (request.action == RequestAction::Follow ? "follow"
                    : request.action == RequestAction::Wave ? "wave" : "assist")
                || !request.acceptedMs || request.expiresMs <= request.acceptedMs
                || request.expiresMs - request.acceptedMs
                    != (request.action == RequestAction::Follow ? 120000u : 30000u)
                || objective.cooperation.state != CooperationState::None
                || objective.checkpoint != QuestProgress{} || objective.information != InformationSearch{}
                || (!Terminal(objective.state) && request.action == RequestAction::Follow && ++following > 1))
                return false;
        }
        auto const& information = objective.information;
        if (information.status > InformationStatus::Lead || information.attempts > 2
            || information.question.size() > 255 || !IsBoundedText(information.question, 255)
            || (information.attempts && (information.status == InformationStatus::None || information.question.empty()
                || information.expiresMs < information.askedMs
                || information.expiresMs - information.askedMs != 120000
                || information.deliveredMs > information.expiresMs))
            || (!information.attempts && information != InformationSearch{})
            || (information.status == InformationStatus::Lead && (!information.leadReport
                || std::find(objective.evidence.begin(), objective.evidence.end(), information.leadReport)
                    == objective.evidence.end()))
            || (information.status != InformationStatus::Lead && information.leadReport)
            || ((information.status == InformationStatus::Pending
                || information.status == InformationStatus::Undelivered) && information.deliveredMs)
            || ((information.status == InformationStatus::Awaiting || information.status == InformationStatus::Lead)
                && information.deliveredMs < information.askedMs))
            return false;
        if (!id || id != objective.id || id >= snapshot.nextId
            || (!objective.quest && !objective.place && !objective.request)
            || !objective.revision
            || objective.revision >= std::numeric_limits<uint64_t>::max() - 1 || objective.lastSampleMs
            || objective.state > ObjectiveState::Cancelled || objective.step > ObjectiveStep::Wait
            || objective.obstruction > Obstruction::Executor || objective.outcome.empty()
            || !IsBoundedText(objective.outcome, 512) || !IsBoundedText(objective.reason, 512)
            || objective.approach.empty() || !IsBoundedText(objective.approach, 64)
            || (objective.person && !IsValidActor(*objective.person)) || objective.evidence.size() > 16
            || objective.attemptsInCircumstances > objective.attempts
            || objective.attempts == std::numeric_limits<uint32_t>::max())
            return false;
        if (!Terminal(objective.state) && !objective.request
            && !(objective.quest ? unfinishedQuests.insert(objective.quest).second
            : unfinishedPlaces.emplace(objective.place, objective.purpose, objective.person).second))
            return false;
        if ((objective.state == ObjectiveState::Active || objective.state == ObjectiveState::Waiting
            || objective.state == ObjectiveState::Blocked) && ++current > 1)
            return false;
        std::set<uint64_t> ancestors{id};
        auto parent = objective.parent;
        while (parent)
        {
            auto found = snapshot.objectives.find(parent);
            if (found == snapshot.objectives.end() || !ancestors.insert(parent).second || ancestors.size() > 3)
                return false;
            parent = found->second.parent;
        }
    }
    return !preparing || (!current && !following && !cooperating);
}

bool ObjectiveBook::CanAsk(uint64_t id, uint64_t now) const
{
    auto const* objective = Find(id);
    if (!objective || objective->purpose != PlacePurpose::Work || objective->state != ObjectiveState::Deferred
        || (objective->obstruction != Obstruction::Information && objective->obstruction != Obstruction::Companions)
        || objective->information.attempts >= 2 || objective->revision >= std::numeric_limits<uint64_t>::max() - 2
        || now > std::numeric_limits<uint64_t>::max() - 120000)
        return false;
    if (objective->obstruction == Obstruction::Companions)
    {
        auto const& cooperation = objective->cooperation;
        if (!objective->quest || !objective->checkpoint.inLog || objective->checkpoint.readyToReward
            || objective->checkpoint.failed || objective->checkpoint.rewarded
            || (cooperation.state != CooperationState::None && cooperation.state != CooperationState::Recruiting
                && (cooperation.state != CooperationState::Deferred || cooperation.attempts >= 2
                    || now < cooperation.reconsiderMs)))
            return false;
    }
    auto const& information = objective->information;
    return !information.attempts || (now >= information.askedMs && now - information.askedMs >= 600000);
}

bool ObjectiveBook::Ask(uint64_t id, uint64_t revision, std::string question, uint64_t now)
{
    if (!CanAsk(id, now) || question.empty() || question.size() > 255 || !IsBoundedText(question, 255))
        return false;
    auto& objective = _objectives.at(id);
    if (objective.revision != revision)
        return false;
    auto const attempts = objective.information.attempts + 1;
    objective.information = {InformationStatus::Pending, attempts, now, now + 120000, 0, 0, std::move(question)};
    ++objective.revision;
    return true;
}

bool ObjectiveBook::QuestionDelivery(uint64_t id, uint32_t attempt, bool delivered, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end())
        return false;
    auto& objective = found->second;
    auto& information = objective.information;
    if (objective.revision >= std::numeric_limits<uint64_t>::max() - 2
        || information.status != InformationStatus::Pending || information.attempts != attempt
        || now < information.askedMs || now >= information.expiresMs)
        return false;
    information.status = delivered ? InformationStatus::Awaiting : InformationStatus::Undelivered;
    information.deliveredMs = delivered ? now : 0;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::InformationLead(uint64_t id, uint32_t attempt, uint64_t report, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !report)
        return false;
    auto& objective = found->second;
    auto& information = objective.information;
    if (objective.revision >= std::numeric_limits<uint64_t>::max() - 2
        || Terminal(objective.state) || information.status != InformationStatus::Awaiting
        || information.attempts != attempt || now < information.askedMs || now >= information.expiresMs
        || objective.evidence.size() >= 16
        || std::find(objective.evidence.begin(), objective.evidence.end(), report) != objective.evidence.end())
        return false;
    information.status = InformationStatus::Lead;
    information.leadReport = report;
    objective.evidence.push_back(report);
    objective.nextReconsiderationMs = now;
    objective.attemptsInCircumstances = 0;
    objective.reason = "Received an uncertain lead; verify it through ordinary gameplay";
    ++objective.revision;
    return true;
}

std::optional<uint64_t> ObjectiveBook::InvestigateLead(uint64_t id, uint32_t attempt, uint64_t report,
    uint32_t place, std::string name, uint64_t now)
{
    auto const* original = Find(id);
    if (!original || !place || original->place == place || now > std::numeric_limits<uint64_t>::max() - _policy.retryMs)
        return std::nullopt;
    auto staged = *this;
    if (!staged.InformationLead(id, attempt, report, now))
        return std::nullopt;
    auto const* proposed = staged.ProposePlace(place, "Investigate reported work in " + name,
        "A speaker offered an uncertain lead; verify work through ordinary gameplay");
    if (!proposed || proposed->evidence.size() >= 16
        || std::find(proposed->evidence.begin(), proposed->evidence.end(), report) != proposed->evidence.end()
        || proposed->revision >= std::numeric_limits<uint64_t>::max() - 2)
        return std::nullopt;
    auto const childId = proposed->id;
    auto& child = staged._objectives.at(childId);
    if (!child.parent)
        child.parent = id;
    child.evidence.push_back(report);
    child.nextReconsiderationMs = now;
    child.attemptsInCircumstances = 0;
    ++child.revision;
    // Keep the exhausted parent deferred while the alternative is investigated.
    staged._objectives.at(id).nextReconsiderationMs = now + _policy.retryMs;
    if (!IsValidObjectiveSnapshot(staged.Capture(), _policy))
        return std::nullopt;
    *this = std::move(staged);
    return childId;
}

bool ObjectiveBook::SetCooperation(uint64_t id, uint64_t revision, Cooperation cooperation)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.revision != revision || !IsValidCooperation(cooperation)
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2
        || (cooperation.state != CooperationState::None
            && (cooperation.objective != id || cooperation.quest != found->second.quest)))
        return false;
    if (found->second.cooperation == cooperation)
        return true;
    if (Preparing() && cooperation.state > CooperationState::None && cooperation.state < CooperationState::Completed)
        return false;
    if (found->second.cooperation.state != CooperationState::None && cooperation.state == CooperationState::None)
        return false; // A reset must not refund recruitment attempts.
    if (cooperation.state > CooperationState::None && cooperation.state < CooperationState::Completed)
        for (auto const& [otherId, objective] : _objectives)
            if (otherId != id && objective.cooperation.state > CooperationState::None
                && objective.cooperation.state < CooperationState::Completed)
                return false;
    found->second.cooperation = std::move(cooperation);
    ++found->second.revision;
    return true;
}

void ObjectiveBook::ExpireQuestions(uint64_t now)
{
    for (auto& [id, objective] : _objectives)
    {
        auto& information = objective.information;
        if ((information.status == InformationStatus::Pending || information.status == InformationStatus::Awaiting)
            && now >= information.expiresMs && objective.revision < std::numeric_limits<uint64_t>::max() - 2)
        {
            information.status = InformationStatus::Unanswered;
            ++objective.revision;
        }
    }
}

bool ObjectiveBook::AwaitCooperation(uint64_t id, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.state == ObjectiveState::Cancelled
        || found->second.cooperation.state != CooperationState::Agreed
        || now < found->second.cooperation.startedMs || now >= found->second.cooperation.deadlineMs
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& objective = found->second;
    if (objective.state == ObjectiveState::Completed)
        return objective.checkpoint.rewarded; // Supporting peers never reopens this owner's earned reward.
    objective.state = ObjectiveState::Deferred;
    objective.obstruction = Obstruction::Companions;
    objective.reason = "Await the matching invitation and actual readiness";
    objective.lastSampleMs = 0;
    objective.nextReconsiderationMs = objective.cooperation.deadlineMs;
    ++objective.revision;
    return true;
}

ObjectiveSnapshot ObjectiveBook::Capture() const
{
    ObjectiveSnapshot snapshot{_nextId, _objectives};
    for (auto& [id, objective] : snapshot.objectives)
        objective.lastSampleMs = 0;
    return snapshot;
}

bool ObjectiveBook::Restore(ObjectiveSnapshot snapshot)
{
    if (!IsValidObjectiveSnapshot(snapshot, _policy))
        return false;
    for (auto& [id, objective] : snapshot.objectives)
        if (objective.request && objective.request->action != RequestAction::Follow && !Terminal(objective.state))
        {
            objective.state = ObjectiveState::Cancelled;
            objective.reason = "An immediate request cannot replay its lost execution after reload";
            ++objective.revision;
        }
        else if (objective.state == ObjectiveState::Active || objective.state == ObjectiveState::Waiting)
        {
            objective.state = ObjectiveState::Waiting;
            objective.step = ObjectiveStep::Wait;
            ++objective.revision;
        }
    _nextId = snapshot.nextId;
    _objectives = std::move(snapshot.objectives);
    return true;
}

bool ObjectiveBook::Reconcile(uint64_t id, QuestProgress const& observed, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest)
        return false;
    auto& objective = found->second;
    if (objective.state == ObjectiveState::Cancelled)
        return true;
    if (objective.state != ObjectiveState::Completed)
        return Observe(id, observed, ObjectiveStep::Wait, now);
    if (observed.rewarded)
        return true;
    objective.state = observed.inLog ? ObjectiveState::Deferred : ObjectiveState::Cancelled;
    objective.checkpoint = observed;
    objective.step = ObjectiveStep::Wait;
    objective.reason = "Saved completion is absent from authoritative quest state after reload";
    objective.attemptsInCircumstances = 0;
    objective.nextReconsiderationMs = observed.inLog ? now : 0;
    ++objective.revision;
    return true;
}

Objective const* ObjectiveBook::ProposeQuest(uint32_t quest, std::string outcome, std::string reason,
    std::optional<QuestProgress> initial)
{
    auto const next = _nextId;
    auto const* objective = quest ? Propose(quest, 0, std::move(outcome), std::move(reason)) : nullptr;
    if (objective && objective->id == next && initial)
        _objectives.at(objective->id).checkpoint = *initial;
    return objective;
}

Objective const* ObjectiveBook::ProposePlace(uint32_t place, std::string outcome, std::string reason)
{
    return place ? Propose(0, place, std::move(outcome), std::move(reason)) : nullptr;
}

Objective const* ObjectiveBook::ProposeActivity(uint32_t place, PlacePurpose purpose, std::string outcome,
    std::string reason, std::optional<ActorKey> companion)
{
    if (!place || purpose == PlacePurpose::Work || purpose > PlacePurpose::Companionship
        || bool(companion) != (purpose == PlacePurpose::Companionship)
        || (companion && (!IsValidActor(*companion) || companion->kind != ActorKind::Player)))
        return nullptr;
    // Retain the completed activity and its cooldown. Repeated candidate generation cannot farm interactions.
    for (auto const& [id, objective] : _objectives)
        if (objective.place == place && objective.purpose == purpose && objective.person == companion
            && objective.state != ObjectiveState::Cancelled)
            return &objective;
    return Propose(0, place, std::move(outcome), std::move(reason), purpose, companion);
}

bool ObjectiveBook::ObserveActivity(uint64_t id, ActivityObservation const& observation, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.purpose == PlacePurpose::Work
        || found->second.state != ObjectiveState::Active || !now || now < found->second.lastSampleMs
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& objective = found->second;
    bool const present = observation.available && observation.area == objective.place;
    uint64_t const elapsed = objective.lastSampleMs && now - objective.lastSampleMs <= 2000
        ? now - objective.lastSampleMs : 0;
    objective.lastSampleMs = now;
    if (!present)
    {
        if (objective.step != ObjectiveStep::Wait)
        {
            objective.step = ObjectiveStep::Wait;
            ++objective.revision;
        }
        return true;
    }
    if (!objective.arrivedMs)
        objective.arrivedMs = now;
    if (objective.purpose == PlacePurpose::Rest && observation.resting && objective.step == ObjectiveStep::Attempt)
        objective.activityMs = std::min(uint64_t(60000), objective.activityMs + elapsed);
    objective.step = observation.resting || objective.purpose != PlacePurpose::Rest
        ? ObjectiveStep::Attempt : ObjectiveStep::Wait;
    bool const complete = objective.purpose == PlacePurpose::Discovery ? observation.discovered
        : objective.purpose == PlacePurpose::Companionship
            ? observation.interaction && observation.person == objective.person : objective.activityMs >= 60000;
    if (complete)
    {
        objective.state = ObjectiveState::Completed;
        objective.completedMs = objective.lastProgressMs = now;
        objective.reason = objective.purpose == PlacePurpose::Discovery ? "Observed a new part of my surroundings"
            : objective.purpose == PlacePurpose::Companionship ? "An ordinary interaction reached my companion"
            : "Observed a minute of stationary rest";
    }
    ++objective.revision;
    return true;
}

bool ObjectiveBook::ReconsiderActivity(uint64_t id, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.purpose == PlacePurpose::Work
        || found->second.purpose == PlacePurpose::Discovery || found->second.state != ObjectiveState::Completed
        || now < found->second.completedMs || now - found->second.completedMs < 600000
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& objective = found->second;
    objective.state = ObjectiveState::Proposed;
    objective.activityMs = objective.completedMs = objective.arrivedMs = objective.lastSampleMs = 0;
    objective.step = ObjectiveStep::Select;
    objective.reason = "Reconsider this activity after the previous observed outcome and cooldown";
    ++objective.revision;
    return true;
}

bool ObjectiveBook::Replan(uint64_t id, std::string reason, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.request || found->second.preparation
        || found->second.cooperation.state != CooperationState::None
        || (found->second.state != ObjectiveState::Active && found->second.state != ObjectiveState::Waiting)
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2
        || now > std::numeric_limits<uint64_t>::max() - 30000
        || reason.empty() || !IsBoundedText(reason, 512))
        return false;
    auto& objective = found->second;
    objective.state = ObjectiveState::Deferred;
    objective.step = ObjectiveStep::Wait;
    objective.reason = std::move(reason);
    objective.nextReconsiderationMs = now + 30000;
    objective.lastSampleMs = 0;
    objective.attemptsInCircumstances = 0;
    ++objective.revision;
    return true;
}

Objective const* ObjectiveBook::Following() const
{
    for (auto const& [id, objective] : _objectives)
        if (objective.request && objective.request->action == RequestAction::Follow && !Terminal(objective.state))
            return &objective;
    return nullptr;
}

std::optional<uint64_t> ObjectiveBook::Request(HumanRequest request)
{
    if (auto const* following = Following(); following && request.action == RequestAction::Follow
        && following->person != request.source.actor)
        return std::nullopt;
    auto staged = *this;
    if (request.action == RequestAction::Follow)
    {
        if (auto const* preparing = staged.Preparing())
            staged.DeferPreparation(preparing->id, "A human request superseded resource preparation",
                request.acceptedMs);
        if (auto const* following = staged.Following())
            if (!staged.Cancel(following->id, "A fresh request renewed the bounded accompaniment"))
                return std::nullopt;
        if (auto const* current = staged.Current())
        {
            if (!staged.Block(current->id, Obstruction::Executor,
                "Accompany the requesting player first", request.acceptedMs)
                || !staged.Defer(current->id, request.acceptedMs))
                return std::nullopt;
        }
        for (auto const& [id, objective] : staged.All())
            if (objective.cooperation.state > CooperationState::None
                && objective.cooperation.state < CooperationState::Completed)
                return std::nullopt;
    }
    // Propose supplies the same bounded collection and eviction policy as quest/place intentions.
    auto const* inserted = staged.Propose(0, 0, "Respond to " + request.source.name,
        "A delivered human request");
    if (!inserted || inserted->request)
        return std::nullopt;
    auto const id = inserted->id;
    auto& objective = staged._objectives.at(id);
    objective.person = request.source.actor;
    objective.approach = request.action == RequestAction::Follow ? "follow" : request.action == RequestAction::Wave
        ? "wave" : "assist";
    objective.request = std::move(request);
    if (!IsValidObjectiveSnapshot(staged.Capture(), _policy))
        return std::nullopt;
    *this = std::move(staged);
    return id;
}

bool ObjectiveBook::ObserveRequest(uint64_t id, RequestObservation const& observation, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.request || Terminal(found->second.state))
        return false;
    auto& objective = found->second;
    auto const& request = *objective.request;
    if (now < request.acceptedMs || objective.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    if (!observation.available)
        return Cancel(id, "The requesting player or compatible control is no longer available");
    bool const follow = request.action == RequestAction::Follow;
    if (now >= request.expiresMs && (!follow || !objective.arrivedMs || !observation.near || observation.recovering))
        return Cancel(id, "The request expired before its outcome was observed");
    auto const before = objective;
    if (follow)
    {
        objective.state = observation.recovering ? ObjectiveState::Waiting : ObjectiveState::Active;
        objective.step = observation.recovering ? ObjectiveStep::Recover : ObjectiveStep::Travel;
        if (!observation.recovering && observation.near && !objective.arrivedMs)
            objective.arrivedMs = objective.lastProgressMs = now;
        objective.reason = observation.recovering ? "Recover before resuming the bounded accompaniment"
            : "Accompany the requesting player through ordinary movement";
    }
    if ((follow && now >= request.expiresMs) || (!follow && observation.effect))
    {
        objective.state = ObjectiveState::Completed;
        objective.step = ObjectiveStep::Wait;
        objective.lastProgressMs = now;
        objective.reason = follow ? "The bounded accompaniment ended after observed proximity"
            : request.action == RequestAction::Wave ? "The requesting player received the wave"
            : "Own engagement against the requested threat was observed";
    }
    if (objective != before)
        ++objective.revision;
    return true;
}

Objective const* ObjectiveBook::Propose(uint32_t quest, uint32_t place, std::string outcome, std::string reason,
    PlacePurpose purpose, std::optional<ActorKey> companion)
{
    if (outcome.empty() || !IsBoundedText(outcome, 512) || !IsBoundedText(reason, 512))
        return nullptr;
    for (auto const& [id, objective] : _objectives)
        if ((quest || place) && objective.quest == quest && (quest || (objective.place == place
            && objective.purpose == purpose && objective.person == companion))
            && !Terminal(objective.state))
            return &objective;
    if (_objectives.size() >= _policy.maxObjectives)
    {
        auto const oldest = std::find_if(_objectives.begin(), _objectives.end(), [this](auto const& entry)
        {
            auto const phase = entry.second.cooperation.state;
            return Terminal(entry.second.state)
                && (phase == CooperationState::None || phase >= CooperationState::Completed)
                && std::none_of(_objectives.begin(), _objectives.end(),
                [&entry](auto const& child) { return child.second.parent == entry.first; });
        });
        if (oldest != _objectives.end())
            _objectives.erase(oldest);
    }
    if (_objectives.size() >= _policy.maxObjectives || _nextId == std::numeric_limits<uint64_t>::max())
        return nullptr;
    Objective objective;
    objective.id = _nextId++;
    objective.quest = quest;
    objective.place = place;
    objective.purpose = purpose;
    objective.person = companion;
    objective.approach = quest ? "pursue_quest" : ActivityCapability(purpose);
    objective.outcome = std::move(outcome);
    objective.reason = std::move(reason);
    return &_objectives.emplace(objective.id, std::move(objective)).first->second;
}

bool ObjectiveBook::Retryable(Objective const& objective, uint64_t now, uint64_t circumstances) const
{
    if (objective.request)
        return false;
    // A route failure says nothing about whether the quest can ever be done. Keep bounded probes
    // available even after the semantic attempt cap; equipment changes do not repair a route.
    if (objective.obstruction == Obstruction::Navigation)
        return objective.state == ObjectiveState::Deferred && now >= objective.nextReconsiderationMs;
    if (objective.quest && (objective.obstruction == Obstruction::Strength
        || objective.obstruction == Obstruction::Supplies || objective.obstruction == Obstruction::Prerequisite))
        return false; // Elapsed time or an unrelated equipment change is not evidence that this condition was resolved.
    if (objective.quest && objective.state == ObjectiveState::Deferred
        && objective.obstruction == Obstruction::Companions)
        return objective.checkpoint.readyToReward || objective.cooperation.state == CooperationState::Working;
    return objective.state == ObjectiveState::Deferred
        && (circumstances != objective.circumstances
            || (objective.attemptsInCircumstances < _policy.maxAttempts && now >= objective.nextReconsiderationMs));
}

bool ObjectiveBook::Activate(uint64_t id, uint64_t revision, QuestProgress const& observed,
    uint64_t now, uint64_t circumstances)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest || !observed.inLog || observed.failed || observed.rewarded)
        return false;
    auto& objective = found->second;
    if (!Start(objective, revision, now, circumstances))
        return false;
    objective.checkpoint = observed;
    return true;
}

bool ObjectiveBook::Prefer(uint64_t id, uint64_t revision, std::string reason, uint64_t report, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.revision != revision || Terminal(found->second.state)
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2
        || !now || reason.empty() || !IsBoundedText(reason, 512))
        return false;
    auto& objective = found->second;
    bool const add = report && std::find(objective.evidence.begin(), objective.evidence.end(), report)
        == objective.evidence.end();
    if (add && objective.evidence.size() >= 16)
        return false;
    if (add)
        objective.evidence.push_back(report);
    objective.plannedMs = now;
    objective.reason = std::move(reason);
    ++objective.revision;
    return true;
}

Objective const* ObjectiveBook::Preferred(uint64_t now, uint64_t circumstances) const
{
    Objective const* choice = nullptr;
    for (auto const& [id, objective] : _objectives)
        if (objective.plannedMs && objective.plannedMs <= now
            && (objective.state == ObjectiveState::Proposed || Retryable(objective, now, circumstances))
            && (!choice || objective.plannedMs > choice->plannedMs))
            choice = &objective;
    return choice;
}

bool ObjectiveBook::ActivatePlace(uint64_t id, uint64_t revision, uint64_t now, uint64_t circumstances)
{
    auto found = _objectives.find(id);
    return found != _objectives.end() && !found->second.quest && found->second.place
        && Start(found->second, revision, now, circumstances);
}

bool ObjectiveBook::Start(Objective& objective, uint64_t revision, uint64_t now, uint64_t circumstances)
{
    if (Preparing() || objective.revision != revision || (Current() && Current()->id != objective.id))
        return false;
    bool const resume = objective.state == ObjectiveState::Waiting;
    if (!resume && objective.state != ObjectiveState::Proposed && !Retryable(objective, now, circumstances))
        return false;
    objective.state = ObjectiveState::Active;
    objective.step = ObjectiveStep::Select;
    objective.obstruction = Obstruction::None;
    objective.lastSampleMs = now;
    if (!resume)
    {
        if (objective.circumstances != circumstances)
            objective.attemptsInCircumstances = 0;
        ++objective.attempts;
        ++objective.attemptsInCircumstances;
        objective.activeWithoutProgressMs = 0;
        objective.arrivedMs = 0;
        objective.discoveredQuest = 0;
    }
    objective.circumstances = circumstances;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::Observe(uint64_t id, QuestProgress const& observed, ObjectiveStep step, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest || Terminal(found->second.state)
        || step > ObjectiveStep::Wait)
        return false;
    auto& objective = found->second;
    auto const& cooperation = objective.cooperation;
    bool const waitingForShare = !objective.checkpoint.inLog && cooperation.owner != cooperation.leader
        && (cooperation.state == CooperationState::Agreed || cooperation.state == CooperationState::Rendezvous)
        && now >= cooperation.startedMs && now < cooperation.deadlineMs;
    bool const unsharedOffer = !objective.checkpoint.inLog && cooperation.owner != cooperation.leader
        && (cooperation.state == CooperationState::Agreed || cooperation.state == CooperationState::Rendezvous
            || cooperation.state == CooperationState::Deferred);
    uint32_t gained = 0;
    for (std::size_t index = 0; index < observed.counters.size(); ++index)
        if (observed.counters[index] > objective.checkpoint.counters[index])
            gained += observed.counters[index] - objective.checkpoint.counters[index];
    bool const progress = gained || (observed.readyToReward && !objective.checkpoint.readyToReward);
    // Only contiguous, bounded active samples count. Login gaps, travel, recovery and waiting are not attempts.
    if (objective.state == ObjectiveState::Active && step == objective.step
        && (step == ObjectiveStep::Attempt || step == ObjectiveStep::TurnIn)
        && now >= objective.lastSampleMs && now - objective.lastSampleMs <= 2000)
        objective.activeWithoutProgressMs += now - objective.lastSampleMs;
    if (progress)
    {
        objective.gainedCredit += gained;
        objective.lastProgressMs = now;
        objective.activeWithoutProgressMs = 0;
        if (objective.state == ObjectiveState::Deferred)
        {
            objective.attemptsInCircumstances = 0;
            objective.nextReconsiderationMs = now;
            objective.reason = "New quest progress makes another attempt useful";
        }
    }
    if (step == ObjectiveStep::Recover && objective.step != ObjectiveStep::Recover)
        ++objective.deaths;
    bool const changed = objective.checkpoint != observed || objective.step != step;
    objective.checkpoint = observed;
    objective.step = step;
    objective.lastSampleMs = now;
    if (changed)
        ++objective.revision;
    if (observed.rewarded)
    {
        if (objective.preparation && objective.preparation->state == PreparationState::Active)
            DeferPreparation(id, "Quest reward superseded resource preparation", now, true);
        objective.state = ObjectiveState::Completed;
        objective.nextReconsiderationMs = 0;
        objective.reason = "Quest reward observed in the owner's quest state";
        ++objective.revision;
    }
    else if (!observed.inLog && !waitingForShare)
    {
        if (!unsharedOffer)
            return Cancel(id, "Quest is no longer in the owner's quest log");
        DeferCooperation(objective.cooperation, "The agreed quest was not shared before the wait expired", now);
        std::string const reason = "No quest acceptance observed; retain the bounded offer retry";
        if (objective.state != ObjectiveState::Deferred || objective.reason != reason
            || objective.nextReconsiderationMs != objective.cooperation.reconsiderMs)
        {
            objective.state = ObjectiveState::Deferred;
            objective.obstruction = Obstruction::Companions;
            objective.reason = reason;
            objective.nextReconsiderationMs = objective.cooperation.reconsiderMs;
            ++objective.revision;
        }
    }
    else if (observed.failed)
        return Block(id, Obstruction::Prerequisite, "Quest failure observed", now);
    else if (objective.state == ObjectiveState::Active
        && objective.activeWithoutProgressMs >= _policy.noProgressMs)
        return Block(id, Obstruction::Executor, "No new quest credit during active execution", now);
    return true;
}

bool ObjectiveBook::ObservePlace(uint64_t id, uint32_t area, uint32_t newQuest, ObjectiveStep step, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.quest || found->second.purpose != PlacePurpose::Work
        || Terminal(found->second.state)
        || step > ObjectiveStep::Wait)
        return false;
    auto& objective = found->second;
    if (objective.state == ObjectiveState::Active && step == ObjectiveStep::Attempt
        && objective.step == step && now >= objective.lastSampleMs && now - objective.lastSampleMs <= 2000)
        objective.activeWithoutProgressMs += now - objective.lastSampleMs;
    objective.lastSampleMs = now;
    if (objective.step != step)
    {
        if (step == ObjectiveStep::Recover)
            ++objective.deaths;
        objective.step = step;
        ++objective.revision;
    }
    if (objective.state != ObjectiveState::Active || area != objective.place)
        return true;
    if (!objective.arrivedMs && (step == ObjectiveStep::Attempt || newQuest))
    {
        objective.arrivedMs = now;
        objective.lastProgressMs = now;
        objective.activeWithoutProgressMs = 0;
        ++objective.revision;
    }
    if (newQuest)
    {
        objective.discoveredQuest = newQuest;
        objective.state = ObjectiveState::Completed;
        objective.reason = "New work accepted through ordinary interaction in the intended area";
        objective.lastProgressMs = now;
        objective.nextReconsiderationMs = 0;
        ++objective.revision;
    }
    else if (objective.activeWithoutProgressMs >= _policy.noProgressMs)
        return Block(id, Obstruction::Executor, "No new work after active local investigation", now);
    return true;
}

bool ObjectiveBook::ReconcilePlace(uint64_t id, bool discoveredQuestStillKnown, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.quest || found->second.purpose != PlacePurpose::Work)
        return false;
    auto& objective = found->second;
    if (objective.state == ObjectiveState::Completed && !discoveredQuestStillKnown)
    {
        objective.state = ObjectiveState::Deferred;
        objective.reason = "Previously discovered work is absent from the authoritative quest state";
        objective.nextReconsiderationMs = now;
        objective.attemptsInCircumstances = 0;
        objective.discoveredQuest = 0;
        ++objective.revision;
    }
    return true;
}

bool ObjectiveBook::ProposeRepair(uint64_t id)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest || found->second.state != ObjectiveState::Deferred
        || found->second.obstruction != Obstruction::Supplies
        || (found->second.preparation && found->second.preparation->state != PreparationState::Completed
            && (found->second.preparation->kind == PreparationKind::RepairEquipment
                || found->second.preparation->state == PreparationState::Active)))
        return false;
    found->second.preparation = ResourcePreparation{};
    ++found->second.revision;
    return true;
}

Objective const* ObjectiveBook::Preparing() const
{
    for (auto const& [id, objective] : _objectives)
        if (objective.preparation && objective.preparation->state == PreparationState::Active)
            return &objective;
    return nullptr;
}

bool ObjectiveBook::CanRepair(uint64_t id, uint64_t now) const
{
    auto const* objective = Find(id);
    if (!objective || !objective->preparation || objective->state != ObjectiveState::Deferred
        || objective->preparation->kind != PreparationKind::RepairEquipment
        || objective->obstruction != Obstruction::Supplies || !objective->checkpoint.inLog
        || objective->checkpoint.failed || objective->checkpoint.readyToReward || objective->checkpoint.rewarded
        || Current() || Following() || Preparing())
        return false;
    for (auto const& [otherId, other] : _objectives)
        if (other.cooperation.state > CooperationState::None && other.cooperation.state < CooperationState::Completed)
            return false;
    auto const& preparation = *objective->preparation;
    return preparation.attemptsInCircumstances < 3 && preparation.attempts < std::numeric_limits<uint32_t>::max() - 2
        && now >= preparation.reconsiderMs
        && (preparation.state == PreparationState::Proposed || preparation.state == PreparationState::Deferred);
}

bool ObjectiveBook::BeginRepair(uint64_t id, uint64_t revision, uint64_t now)
{
    if (!now || now > std::numeric_limits<uint64_t>::max() - 720000 || !CanRepair(id, now)
        || Find(id)->revision != revision || revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& objective = _objectives.at(id);
    auto& preparation = *objective.preparation;
    preparation.state = PreparationState::Active;
    preparation.startedMs = now;
    preparation.deadlineMs = now + 120000;
    preparation.reconsiderMs = 0;
    preparation.reason = "Approach a visible repairer and use my own funds to repair equipped items";
    ++preparation.attempts;
    ++preparation.attemptsInCircumstances;
    preparation.fundsAtAttempt = preparation.ownMoney;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::ObserveRepair(uint64_t id, bool needed, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation
        || found->second.preparation->kind != PreparationKind::RepairEquipment)
        return false;
    auto& objective = found->second;
    auto& preparation = *objective.preparation;
    if ((Terminal(objective.state) || !objective.checkpoint.inLog || objective.checkpoint.failed
        || objective.checkpoint.readyToReward) && preparation.state == PreparationState::Active)
        return DeferPreparation(id, "The parent quest no longer needs preparation", now, true);
    if (!needed && preparation.state != PreparationState::Completed)
    {
        preparation.state = PreparationState::Completed;
        preparation.reason = "Observed equipped items no longer have critically low durability";
        ++objective.revision;
        return true;
    }
    if (preparation.state == PreparationState::Active && now >= preparation.deadlineMs)
        return DeferPreparation(id, "No equipment recovery observed before the preparation deadline", now);
    return false;
}

bool ObjectiveBook::ProposeSupplies(uint64_t id, uint32_t item, uint32_t count)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest || Terminal(found->second.state) || !item || !count
        || (found->second.preparation && found->second.preparation->state != PreparationState::Completed))
        return false;
    ResourcePreparation preparation;
    preparation.kind = PreparationKind::BuyQuestSupplies;
    preparation.item = item;
    preparation.count = count;
    preparation.reason = "A received merchant offer can supply an item required by my accepted quest";
    found->second.preparation = std::move(preparation);
    ++found->second.revision;
    return true;
}

bool ObjectiveBook::CanBuySupplies(uint64_t id, uint64_t now) const
{
    auto const* objective = Find(id);
    if (!objective || !objective->preparation || Terminal(objective->state) || !objective->checkpoint.inLog
        || objective->checkpoint.failed || objective->checkpoint.readyToReward || objective->checkpoint.rewarded
        || objective->preparation->kind != PreparationKind::BuyQuestSupplies || Following() || Preparing()
        || (Current() && Current()->id != id))
        return false;
    for (auto const& [otherId, other] : _objectives)
        if (other.cooperation.state > CooperationState::None && other.cooperation.state < CooperationState::Completed)
            return false;
    auto const& preparation = *objective->preparation;
    return preparation.attemptsInCircumstances < 3 && preparation.attempts < std::numeric_limits<uint32_t>::max() - 2
        && now >= preparation.reconsiderMs
        && (preparation.state == PreparationState::Proposed || preparation.state == PreparationState::Deferred);
}

bool ObjectiveBook::BeginSupplyPurchase(uint64_t id, uint64_t revision, uint64_t now)
{
    if (!now || now > std::numeric_limits<uint64_t>::max() - 720000 || !CanBuySupplies(id, now)
        || Find(id)->revision != revision || revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& objective = _objectives.at(id);
    auto& preparation = *objective.preparation;
    preparation.state = PreparationState::Active;
    preparation.startedMs = now;
    preparation.deadlineMs = now + 120000;
    preparation.reconsiderMs = 0;
    preparation.reason = "Buy the required quest item from a currently received ordinary merchant offer";
    ++preparation.attempts;
    ++preparation.attemptsInCircumstances;
    preparation.fundsAtAttempt = preparation.ownMoney;
    objective.state = ObjectiveState::Deferred;
    objective.step = ObjectiveStep::Wait;
    objective.nextReconsiderationMs = now;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::ObserveSupplies(uint64_t id, uint32_t ownItemCount, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation
        || found->second.preparation->kind != PreparationKind::BuyQuestSupplies)
        return false;
    auto& objective = found->second;
    auto& preparation = *objective.preparation;
    if ((Terminal(objective.state) || !objective.checkpoint.inLog || objective.checkpoint.failed)
        && preparation.state == PreparationState::Active)
        return DeferPreparation(id, "The parent quest no longer needs these supplies", now, true);
    if (ownItemCount >= preparation.count && preparation.state != PreparationState::Completed)
    {
        preparation.state = PreparationState::Completed;
        preparation.reason = "Observed the required quantity in my own inventory";
        ++objective.revision;
        return true;
    }
    if (preparation.state == PreparationState::Active && now >= preparation.deadlineMs)
        return DeferPreparation(id, "Required supplies were not acquired before the preparation deadline", now);
    return false;
}

bool ObjectiveBook::ObservePreparationFunds(uint64_t id, uint32_t ownMoney, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation)
        return false;
    auto& preparation = *found->second.preparation;
    if (preparation.fundsKnown && preparation.ownMoney == ownMoney)
        return false;
    if (preparation.fundsKnown && preparation.state == PreparationState::Deferred
        && ownMoney > preparation.fundsAtAttempt)
    {
        preparation.attemptsInCircumstances = 0;
        preparation.reconsiderMs = now;
        preparation.reason = "Observed more own money than at the failed attempt; reconsider affordable preparation";
    }
    if (!preparation.fundsKnown)
        preparation.fundsAtAttempt = ownMoney; // Old snapshots cannot claim an unobserved increase on first load.
    preparation.ownMoney = ownMoney;
    preparation.fundsKnown = true;
    ++found->second.revision;
    return true;
}

bool ObjectiveBook::PreparationTransaction(uint64_t id, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation)
        return false;
    auto& preparation = *found->second.preparation;
    if (preparation.state != PreparationState::Active || now < preparation.startedMs || now >= preparation.deadlineMs
        || preparation.lastTransactionMs >= preparation.startedMs || preparation.transactions >= preparation.attempts)
        return false;
    // Charge before the effect. Reload may re-observe equipment, but cannot repeat this attempt's transaction.
    preparation.lastTransactionMs = now;
    ++preparation.transactions;
    ++found->second.revision;
    return true;
}

bool ObjectiveBook::PreparationExpense(uint64_t id, uint32_t before, uint32_t after)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation || after >= before)
        return false;
    auto& preparation = *found->second.preparation;
    if (preparation.state != PreparationState::Active || preparation.lastTransactionMs < preparation.startedMs)
        return false;
    preparation.spentMoney += before - after;
    if (preparation.fundsKnown)
        preparation.ownMoney = after;
    ++found->second.revision;
    return true;
}

bool ObjectiveBook::PreparationIncome(uint64_t id, uint32_t before, uint32_t after)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation || after <= before)
        return false;
    auto& preparation = *found->second.preparation;
    if (preparation.state != PreparationState::Active || preparation.lastTransactionMs < preparation.startedMs)
        return false;
    preparation.earnedMoney += after - before;
    if (preparation.fundsKnown)
        preparation.ownMoney = after;
    ++found->second.revision;
    return true;
}

bool ObjectiveBook::DeferPreparation(uint64_t id, std::string reason, uint64_t now, bool cancelled)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.preparation || reason.empty() || !IsBoundedText(reason, 512)
        || now > std::numeric_limits<uint64_t>::max() - 600000)
        return false;
    auto& preparation = *found->second.preparation;
    if (preparation.state != PreparationState::Active)
        return false;
    preparation.state = cancelled ? PreparationState::Cancelled : PreparationState::Deferred;
    preparation.reason = std::move(reason);
    preparation.reconsiderMs = now + 600000;
    ++found->second.revision;
    return true;
}

char const* Name(PreparationState value)
{
    switch (value)
    {
        case PreparationState::Proposed: return "proposed";
        case PreparationState::Active: return "active";
        case PreparationState::Completed: return "completed";
        case PreparationState::Deferred: return "deferred";
        case PreparationState::Cancelled: return "cancelled";
    }
    return "invalid";
}

bool ObjectiveBook::Block(uint64_t id, Obstruction reason, std::string explanation, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || Terminal(found->second.state) || reason == Obstruction::None
        || reason > Obstruction::Executor || !IsBoundedText(explanation, 512))
        return false;
    auto& objective = found->second;
    bool const readiness = reason == Obstruction::Strength || reason == Obstruction::Supplies
        || reason == Obstruction::Prerequisite;
    if ((objective.state == ObjectiveState::Blocked || objective.state == ObjectiveState::Deferred)
        && objective.obstruction != reason
        && (readiness || (objective.state == ObjectiveState::Blocked
            && objective.obstruction == Obstruction::Executor)))
    {
        objective.obstruction = reason;
        objective.reason = std::move(explanation);
        ++objective.revision;
        return true; // Fresh readiness or a specific cause refines the diagnosis without refunding time or attempts.
    }
    if (objective.state == ObjectiveState::Blocked || objective.state == ObjectiveState::Deferred)
        return false;
    // Observations also reconcile background quests. Their failure must not steal execution ownership.
    auto const* current = Current();
    objective.state = current && current->id != id ? ObjectiveState::Deferred : ObjectiveState::Blocked;
    objective.obstruction = reason;
    objective.reason = std::move(explanation);
    objective.nextReconsiderationMs = now + _policy.retryMs;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::ResolveReadiness(uint64_t id, QuestReadiness const& readiness, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest || found->second.state != ObjectiveState::Deferred
        || QuestReadinessObstruction(readiness) != Obstruction::None || readiness.rewarded
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2)
        return false;
    auto& objective = found->second;
    if (objective.obstruction != Obstruction::Strength && objective.obstruction != Obstruction::Supplies
        && objective.obstruction != Obstruction::Prerequisite)
        return false;
    objective.obstruction = Obstruction::None;
    objective.reason = "Own level, equipment and quest conditions now permit another attempt";
    objective.nextReconsiderationMs = now;
    objective.attemptsInCircumstances = 0;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::ObserveOpportunity(uint64_t id, QuestOpportunity opportunity, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || !found->second.quest || opportunity > QuestOpportunity::Respawn
        || found->second.revision >= std::numeric_limits<uint64_t>::max() - 2
        || now > std::numeric_limits<uint64_t>::max() - 60000)
        return false;
    auto& objective = found->second;
    if (objective.state == ObjectiveState::Waiting && objective.obstruction == Obstruction::Competition)
    {
        if (opportunity == QuestOpportunity::Available)
        {
            objective.state = ObjectiveState::Active;
            objective.obstruction = Obstruction::None;
            objective.step = ObjectiveStep::Attempt;
            objective.reason = "An eligible quest target is now visible";
            objective.nextReconsiderationMs = 0;
            objective.lastSampleMs = now;
        }
        else if (now >= objective.nextReconsiderationMs)
        {
            objective.state = ObjectiveState::Blocked;
            objective.reason = "No eligible target became available during the bounded wait";
        }
        else
            return false;
    }
    else if (objective.state == ObjectiveState::Active
        && (opportunity == QuestOpportunity::Tagged || opportunity == QuestOpportunity::Respawn))
    {
        objective.state = ObjectiveState::Waiting;
        objective.step = ObjectiveStep::Wait;
        objective.obstruction = Obstruction::Competition;
        objective.reason = opportunity == QuestOpportunity::Tagged
            ? "Visible quest creatures are already claimed; wait briefly for an eligible target"
            : "Only matching corpses are visible; wait briefly for a respawn";
        objective.nextReconsiderationMs = now + 60000;
        objective.lastSampleMs = now;
    }
    else
        return false;
    ++objective.revision;
    return true;
}

void ObjectiveBook::ReconsiderNavigation(uint64_t now)
{
    for (auto& [id, objective] : _objectives)
        if (!objective.request && objective.state == ObjectiveState::Deferred
            && objective.obstruction == Obstruction::Navigation)
        {
            objective.nextReconsiderationMs = now;
            objective.attemptsInCircumstances = 0;
            ++objective.revision;
        }
}

bool ObjectiveBook::Defer(uint64_t id, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.state != ObjectiveState::Blocked)
        return false;
    found->second.state = ObjectiveState::Deferred;
    auto const& objective = found->second;
    uint64_t delay = _policy.retryMs;
    if (objective.obstruction == Obstruction::Navigation)
        delay = std::min(delay, uint64_t(30000) << std::min(objective.attemptsInCircumstances, 5u));
    found->second.nextReconsiderationMs = now + delay;
    ++found->second.revision;
    return true;
}

bool ObjectiveBook::Suspend(uint64_t id, ObjectiveStep step, std::string reason, uint64_t now)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || found->second.state != ObjectiveState::Active
        || step > ObjectiveStep::Wait || !IsBoundedText(reason, 512))
        return false;
    auto& objective = found->second;
    objective.state = ObjectiveState::Waiting;
    objective.step = step;
    objective.reason = std::move(reason);
    objective.lastSampleMs = now;
    ++objective.revision;
    return true;
}

bool ObjectiveBook::Cancel(uint64_t id, std::string reason)
{
    auto found = _objectives.find(id);
    if (found == _objectives.end() || Terminal(found->second.state) || !IsBoundedText(reason, 512))
        return false;
    found->second.state = ObjectiveState::Cancelled;
    found->second.nextReconsiderationMs = 0;
    found->second.reason = std::move(reason);
    if (found->second.preparation && found->second.preparation->state == PreparationState::Active)
    {
        found->second.preparation->state = PreparationState::Cancelled;
        found->second.preparation->reason = "The parent quest intention was cancelled";
    }
    auto& cooperation = found->second.cooperation;
    if (cooperation.state > CooperationState::None && cooperation.state < CooperationState::Completed)
    {
        cooperation.state = CooperationState::Cancelled;
        cooperation.reason = "The owner's quest intention was cancelled";
    }
    ++found->second.revision;
    return true;
}

Objective const* ObjectiveBook::Find(uint64_t id) const
{
    auto found = _objectives.find(id);
    return found == _objectives.end() ? nullptr : &found->second;
}

Objective const* ObjectiveBook::Current() const
{
    for (auto const& [id, objective] : _objectives)
        if (objective.state == ObjectiveState::Active || objective.state == ObjectiveState::Waiting
            || objective.state == ObjectiveState::Blocked)
            return &objective;
    return nullptr;
}

char const* Name(PlacePurpose value)
{
    switch (value)
    {
        case PlacePurpose::Work: return "work";
        case PlacePurpose::Discovery: return "discovery";
        case PlacePurpose::Rest: return "rest";
        case PlacePurpose::Companionship: return "companionship";
    }
    return "invalid";
}

char const* ActivityCapability(PlacePurpose value)
{
    switch (value)
    {
        case PlacePurpose::Work: return "discover_work";
        case PlacePurpose::Discovery: return "explore_place";
        case PlacePurpose::Rest: return "rest";
        case PlacePurpose::Companionship: return "visit_companion";
    }
    return "invalid";
}

char const* Name(ObjectiveState value)
{
    switch (value)
    {
        case ObjectiveState::Proposed: return "proposed";
        case ObjectiveState::Active: return "active";
        case ObjectiveState::Waiting: return "waiting";
        case ObjectiveState::Blocked: return "blocked";
        case ObjectiveState::Deferred: return "deferred";
        case ObjectiveState::Completed: return "completed";
        case ObjectiveState::Cancelled: return "cancelled";
    }
    return "invalid";
}

char const* Name(ObjectiveStep value)
{
    switch (value)
    {
        case ObjectiveStep::Select: return "select";
        case ObjectiveStep::Travel: return "travel";
        case ObjectiveStep::Attempt: return "attempt";
        case ObjectiveStep::TurnIn: return "turn_in";
        case ObjectiveStep::Recover: return "recover";
        case ObjectiveStep::Wait: return "wait";
    }
    return "invalid";
}

char const* Name(Obstruction value)
{
    switch (value)
    {
        case Obstruction::None: return "none";
        case Obstruction::Strength: return "strength";
        case Obstruction::Companions: return "companions";
        case Obstruction::Information: return "information";
        case Obstruction::Prerequisite: return "prerequisite";
        case Obstruction::Supplies: return "supplies";
        case Obstruction::Competition: return "competition";
        case Obstruction::Navigation: return "navigation";
        case Obstruction::Executor: return "executor";
    }
    return "invalid";
}
char const* Name(InformationStatus value)
{
    switch (value)
    {
        case InformationStatus::None: return "none";
        case InformationStatus::Pending: return "pending";
        case InformationStatus::Undelivered: return "undelivered";
        case InformationStatus::Awaiting: return "awaiting_reply";
        case InformationStatus::Unanswered: return "unanswered";
        case InformationStatus::Lead: return "lead_received";
    }
    return "invalid";
}

}
