/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_OBJECTIVE_PLANNING_H
#define MOD_ALLES_OBJECTIVE_PLANNING_H

#include "bridge/PlanningWire.h"
#include "domain/Knowledge.h"
#include "domain/Objective.h"
#include "domain/Satisfaction.h"
#include <boost/json/object.hpp>

namespace Alles
{
struct ObjectiveOption
{
    std::string capability;
    uint64_t objective = 0;
    uint64_t revision = 0;
    uint32_t quest = 0;
    uint32_t place = 0;
    std::optional<ActorKey> person;
};

struct ObjectivePlanningJob
{
    CapabilityContext issued;
    std::vector<ObjectiveOption> options;
    std::map<std::string, uint64_t> reports;
    uint64_t createdMs = 0;
    boost::json::object context;
    std::optional<uint64_t> satisfactionSelection;
};

struct ObjectiveChoice
{
    std::string status;
    uint64_t selected = 0;
    uint64_t release = 0;
    uint64_t question = 0;
};

// Only the owner's accepted quest rewards/costs and actual balance; projected rewards never authorize spending.
struct QuestFinances
{
    uint32_t ownMoney = 0;
    std::map<uint32_t, int32_t> money;
    std::set<uint32_t> turnInCredit;
};

uint32_t MissingQuestMoney(ObjectiveBook const& book, QuestFinances const& finances);
std::vector<uint64_t> IncomeQuestOrder(ObjectiveBook const& book, QuestFinances const& finances,
    uint64_t circumstances, uint64_t now);

CapabilityRegistry ObjectiveCapabilities();
uint64_t ObjectiveDecisionSignal(ObjectiveBook const& book, PrivateKnowledge const& knowledge,
    uint8_t level, uint32_t area, uint64_t circumstances, uint64_t now,
    std::optional<QuestFinances> const& finances = std::nullopt);
std::optional<ObjectivePlanningJob> PrepareObjectivePlanning(ActorKey owner, uint64_t generation,
    ObjectiveBook const& book, PrivateKnowledge const& knowledge, uint8_t level, uint32_t area,
    uint64_t circumstances, bool canAsk, uint64_t now,
    std::optional<QuestFinances> const& finances = std::nullopt,
    SatisfactionDecision const* satisfaction = nullptr);
ObjectiveChoice ApplyObjectiveChoice(ObjectivePlanningJob const& job, Bridge::PlanningDecision const& decision,
    CapabilityContext const& live, ObjectiveBook& book, PrivateKnowledge const& knowledge,
    uint64_t circumstances, bool canAsk, uint64_t now, SatisfactionDecision const* satisfaction = nullptr);
}
#endif
