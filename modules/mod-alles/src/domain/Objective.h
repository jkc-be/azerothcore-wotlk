/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_OBJECTIVE_H
#define MOD_ALLES_OBJECTIVE_H

#include "Memory.h"
#include "Cooperation.h"
#include <array>
#include <map>
#include <vector>

namespace Alles
{
enum class ObjectiveState : uint8_t
{
    Proposed, Active, Waiting, Blocked, Deferred, Completed, Cancelled
};
enum class Obstruction : uint8_t
{
    None, Strength, Companions, Information, Prerequisite, Supplies, Competition, Navigation, Executor
};
enum class ObjectiveStep : uint8_t
{
    Select, Travel, Attempt, TurnIn, Recover, Wait
};

enum class PlacePurpose : uint8_t
{
    Work, Discovery, Rest, Companionship
};

struct ActivityObservation
{
    uint32_t area = 0;
    bool available = false; // Alive, autonomous, out of combat and at the activity's actual location.
    bool resting = false; // Observed stationary rest, not merely an issued idle command.
    bool discovered = false; // A new personal observation, not arrival or distance traveled.
    bool interaction = false; // Actual comprehended delivery/interaction with the intended companion.
    std::optional<ActorKey> person;
};

enum class InformationStatus : uint8_t
{
    None, Pending, Undelivered, Awaiting, Unanswered, Lead
};

struct InformationSearch
{
    InformationStatus status = InformationStatus::None;
    uint32_t attempts = 0;
    uint64_t askedMs = 0;
    uint64_t expiresMs = 0;
    uint64_t deliveredMs = 0;
    uint64_t leadReport = 0;
    std::string question;

    bool operator==(InformationSearch const&) const = default;
};

struct QuestProgress
{
    // WotLK has four creature/gameobject and six required-item counters. Values are sampled from the owner.
    std::array<uint32_t, 10> counters{};
    bool inLog = false;
    bool readyToReward = false;
    bool rewarded = false;
    bool failed = false;

    bool operator==(QuestProgress const&) const = default;
};

enum class RequestAction : uint8_t
{
    Follow, Wave, Assist
};

struct HumanRequest
{
    RequestAction action = RequestAction::Follow;
    Reference source;
    std::string statement;
    std::string targetName; // A described threat, never a persisted live combat target or permission to reacquire it.
    uint64_t acceptedMs = 0;
    uint64_t expiresMs = 0;

    bool operator==(HumanRequest const&) const = default;
};

struct RequestObservation
{
    bool available = false;
    bool recovering = false;
    bool near = false;
    bool effect = false; // Delivered emote or actual engagement, not an executor return value.
};

struct QuestReadiness
{
    bool rewarded = false;
    bool failed = false;
    bool turningIn = false;
    bool insufficientMoney = false;
    bool criticallyDamagedEquipment = false;
    bool aboveLevelCapability = false;
    bool unsupportedExecutor = false;
    bool needsCompanions = false;
};

// Classifies only supplied own-state evidence; navigation and competition are separate execution observations.
Obstruction QuestReadinessObstruction(QuestReadiness const& readiness);

enum class QuestOpportunity : uint8_t
{
    Unknown, Available, Tagged, Respawn
};
QuestOpportunity ClassifyQuestOpportunity(uint32_t available, uint32_t tagged, uint32_t corpses, bool completeView);

enum class PreparationState : uint8_t
{
    Proposed, Active, Completed, Deferred, Cancelled
};

enum class PreparationKind : uint8_t
{
    RepairEquipment, BuyQuestSupplies
};

struct ResourcePreparation
{
    PreparationState state = PreparationState::Proposed;
    uint32_t attempts = 0;
    uint32_t transactions = 0;
    uint64_t startedMs = 0;
    uint64_t deadlineMs = 0;
    uint64_t reconsiderMs = 0;
    uint64_t lastTransactionMs = 0;
    uint64_t spentMoney = 0;
    std::string reason = "Repair critically damaged equipped items before resuming this quest";
    PreparationKind kind = PreparationKind::RepairEquipment;
    uint32_t item = 0;
    uint32_t count = 0;
    uint32_t attemptsInCircumstances = 0;
    uint32_t ownMoney = 0;
    uint32_t fundsAtAttempt = 0;
    bool fundsKnown = false;
    uint64_t earnedMoney = 0;

    bool operator==(ResourcePreparation const&) const = default;
};

struct Objective
{
    uint64_t id = 0;
    uint64_t revision = 1;
    uint64_t parent = 0;
    uint32_t quest = 0;
    uint32_t place = 0;
    std::optional<ActorKey> person;
    ObjectiveState state = ObjectiveState::Proposed;
    ObjectiveStep step = ObjectiveStep::Select;
    Obstruction obstruction = Obstruction::None;
    std::string outcome;
    std::string reason;
    std::string approach = "pursue_quest";
    std::vector<uint64_t> evidence;
    QuestProgress checkpoint;
    uint64_t lastSampleMs = 0;
    uint64_t lastProgressMs = 0;
    uint64_t activeWithoutProgressMs = 0;
    uint64_t nextReconsiderationMs = 0;
    uint64_t circumstances = 0;
    uint32_t attempts = 0;
    uint32_t attemptsInCircumstances = 0;
    uint32_t deaths = 0;
    uint32_t gainedCredit = 0;
    uint64_t plannedMs = 0; // Latest grounded preference; it never represents execution or progress.
    uint64_t arrivedMs = 0;
    uint32_t discoveredQuest = 0;
    InformationSearch information;
    Cooperation cooperation;
    std::optional<HumanRequest> request;
    std::optional<ResourcePreparation> preparation;
    PlacePurpose purpose = PlacePurpose::Work;
    uint64_t activityMs = 0;
    uint64_t completedMs = 0;

    bool operator==(Objective const&) const = default;
};

struct ObjectiveSnapshot
{
    uint64_t nextId = 1;
    std::map<uint64_t, Objective> objectives;

    bool operator==(ObjectiveSnapshot const&) const = default;
};

struct ObjectivePolicy
{
    uint64_t noProgressMs = 300000;
    uint64_t retryMs = 600000;
    uint32_t maxAttempts = 3;
    std::size_t maxObjectives = 32;
};

bool IsValidObjectiveSnapshot(ObjectiveSnapshot const& snapshot, ObjectivePolicy const& policy = {});

// Per-owner semantic intentions. Caller supplies authoritative observations, never command return values.
class ObjectiveBook
{
public:
    explicit ObjectiveBook(ObjectivePolicy policy = {}) : _policy(policy) { }
    Objective const* ProposeQuest(uint32_t quest, std::string outcome, std::string reason,
        std::optional<QuestProgress> initial = {});
    Objective const* ProposePlace(uint32_t place, std::string outcome, std::string reason);
    Objective const* ProposeActivity(uint32_t place, PlacePurpose purpose, std::string outcome,
        std::string reason, std::optional<ActorKey> companion = std::nullopt);
    bool ObserveActivity(uint64_t id, ActivityObservation const& observation, uint64_t now);
    bool ReconsiderActivity(uint64_t id, uint64_t now);
    bool Replan(uint64_t id, std::string reason, uint64_t now);
    std::optional<uint64_t> Request(HumanRequest request);
    bool ObserveRequest(uint64_t id, RequestObservation const& observation, uint64_t now);
    Objective const* Following() const;
    bool Prefer(uint64_t id, uint64_t revision, std::string reason, uint64_t report, uint64_t now);
    Objective const* Preferred(uint64_t now, uint64_t circumstances) const;
    bool ActivatePlace(uint64_t id, uint64_t revision, uint64_t now, uint64_t circumstances);
    // A new quest must come from the owner's observed quest-log change in this area, never a command result.
    bool ObservePlace(uint64_t id, uint32_t area, uint32_t newQuest, ObjectiveStep step, uint64_t now);
    bool ReconcilePlace(uint64_t id, bool discoveredQuestStillKnown, uint64_t now);
    bool Activate(uint64_t id, uint64_t revision, QuestProgress const& observed,
        uint64_t now, uint64_t circumstances);
    bool Observe(uint64_t id, QuestProgress const& observed, ObjectiveStep step, uint64_t now);
    bool Block(uint64_t id, Obstruction reason, std::string explanation, uint64_t now);
    bool ResolveReadiness(uint64_t id, QuestReadiness const& readiness, uint64_t now);
    bool ObserveOpportunity(uint64_t id, QuestOpportunity opportunity, uint64_t now);
    bool ProposeRepair(uint64_t id);
    bool ProposeSupplies(uint64_t id, uint32_t item, uint32_t count);
    bool CanBuySupplies(uint64_t id, uint64_t now) const;
    bool BeginSupplyPurchase(uint64_t id, uint64_t revision, uint64_t now);
    bool ObserveSupplies(uint64_t id, uint32_t ownItemCount, uint64_t now);
    bool ObservePreparationFunds(uint64_t id, uint32_t ownMoney, uint64_t now);
    bool CanRepair(uint64_t id, uint64_t now) const;
    bool BeginRepair(uint64_t id, uint64_t revision, uint64_t now);
    bool ObserveRepair(uint64_t id, bool needed, uint64_t now);
    bool PreparationTransaction(uint64_t id, uint64_t now);
    bool PreparationExpense(uint64_t id, uint32_t before, uint32_t after);
    bool PreparationIncome(uint64_t id, uint32_t before, uint32_t after);
    bool DeferPreparation(uint64_t id, std::string reason, uint64_t now, bool cancelled = false);
    Objective const* Preparing() const;
    bool Defer(uint64_t id, uint64_t now);
    bool Suspend(uint64_t id, ObjectiveStep step, std::string reason, uint64_t now);
    bool Cancel(uint64_t id, std::string reason);
    bool CanAsk(uint64_t id, uint64_t now) const;
    bool Ask(uint64_t id, uint64_t revision, std::string question, uint64_t now);
    bool QuestionDelivery(uint64_t id, uint32_t attempt, bool delivered, uint64_t now);
    bool InformationLead(uint64_t id, uint32_t attempt, uint64_t report, uint64_t now);
    std::optional<uint64_t> InvestigateLead(uint64_t id, uint32_t attempt, uint64_t report,
        uint32_t place, std::string name, uint64_t now);
    bool SetCooperation(uint64_t id, uint64_t revision, Cooperation cooperation);
    bool AwaitCooperation(uint64_t id, uint64_t now);
    void ExpireQuestions(uint64_t now);
    bool Retryable(Objective const& objective, uint64_t now, uint64_t circumstances) const;
    void ReconsiderNavigation(uint64_t now);
    Objective const* Find(uint64_t id) const;
    Objective const* Current() const;
    std::map<uint64_t, Objective> const& All() const { return _objectives; }
    ObjectiveSnapshot Capture() const;
    // Active execution becomes waiting; no elapsed sample or movement handle is restored.
    bool Restore(ObjectiveSnapshot snapshot);
    // Reload can expose a quest save older than our snapshot. Recheck even a previously observed completion.
    bool Reconcile(uint64_t id, QuestProgress const& observed, uint64_t now);

private:
    Objective const* Propose(uint32_t quest, uint32_t place, std::string outcome, std::string reason,
        PlacePurpose purpose = PlacePurpose::Work, std::optional<ActorKey> companion = std::nullopt);
    bool Start(Objective& objective, uint64_t revision, uint64_t now, uint64_t circumstances);
    ObjectivePolicy _policy;
    std::map<uint64_t, Objective> _objectives;
    uint64_t _nextId = 1;
};

char const* Name(ObjectiveState value);
char const* Name(ObjectiveStep value);
char const* Name(Obstruction value);
char const* Name(InformationStatus value);
char const* Name(PreparationState value);
char const* Name(PlacePurpose value);
char const* ActivityCapability(PlacePurpose value);
}
#endif
