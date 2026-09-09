/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "ObjectiveRuntime.h"
#include "PartyActions.h"
#include "ObjectivePlanning.h"
#include "Item.h"
#include "Bag.h"
#include "Creature.h"
#include "AdvicePlanning.h"
#include "bridge/Service.h"
#include <deque>
#include "ConversationRuntime.h"
#include "perception/SpeechRoute.h"
#include "perception/MerchantInventory.h"
#include "perception/ChatAudience.h"
#include "ChatSender.h"
#include "domain/Capability.h"
#include "domain/Objective.h"
#include "domain/Exploration.h"
#include "telemetry/Recorder.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "PlayerbotAI.h"
#include "AiObjectContext.h"
#include "PlayerbotMgr.h"
#include "NewRpgBaseAction.h"
#include "AttackAction.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "ItemPackets.h"
#include "TargetedMovementGenerator.h"
#include <boost/json.hpp>
#include <algorithm>
#include <limits>
#include <cmath>

namespace Alles
{
namespace
{
class CooperativeMovement final : public NewRpgBaseAction
{
public:
    explicit CooperativeMovement(PlayerbotAI* ai) : NewRpgBaseAction(ai, "alles cooperative movement") { }
    bool Walk(WorldPosition const& destination) { return MoveFarTo(destination); }
};

class CooperativeAssist final : public AttackAction
{
public:
    explicit CooperativeAssist(PlayerbotAI* ai) : AttackAction(ai, "alles cooperative assist") { }
    bool Engage(Unit* target) { return Attack(target); }
};

Player* Find(ActorKey owner)
{
    return ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32_t(owner.id)));
}

QuestProgress Sample(Player& player, uint32_t quest)
{
    static_assert(QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT == 10);
    QuestProgress result;
    result.rewarded = player.GetQuestRewardStatus(quest);
    auto const status = player.GetQuestStatus(quest);
    result.readyToReward = status == QUEST_STATUS_COMPLETE;
    result.failed = status == QUEST_STATUS_FAILED;
    result.inLog = player.FindQuestSlot(quest) < MAX_QUEST_LOG_SIZE;
    auto const found = player.getQuestStatusMap().find(quest);
    if (found != player.getQuestStatusMap().end())
    {
        for (std::size_t index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
            result.counters[index] = found->second.CreatureOrGOCount[index];
        for (std::size_t index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
            result.counters[index + QUEST_OBJECTIVES_COUNT] = found->second.ItemCount[index];
    }
    return result;
}

bool Autonomous(Player& bot, PlayerbotAI& ai)
{
    return bot.IsInWorld() && bot.GetSession() && bot.GetSession()->IsBot()
        && !bot.GetSession()->isLogingOut() && !IsSelfBot(&bot) && !ai.IsExternallyControlled()
        && (CurrentControlMode(bot) == ControlMode::AutonomousSolo
            || CurrentControlMode(bot) == ControlMode::AutonomousParty);
}

std::set<uint32_t> QuestLog(Player const& bot)
{
    std::set<uint32_t> result;
    for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (auto const quest = bot.GetQuestSlotQuestId(slot))
            result.insert(quest);
    return result;
}

uint64_t Circumstances(Player const& bot)
{
    uint64_t result = bot.GetLevel();
    for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        auto const* item = bot.GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        result = result * 1099511628211ULL ^ (item ? item->GetEntry() : 0);
        result = result * 1099511628211ULL ^ (item ? item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) : 0);
    }
    return result;
}

bool HasTurnInCredit(Player& bot, Quest const& quest)
{
    auto const found = bot.getQuestStatusMap().find(quest.GetQuestId());
    if (found == bot.getQuestStatusMap().end() || found->second.Status != QUEST_STATUS_INCOMPLETE)
        return false;
    auto const& status = found->second;
    for (unsigned index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
        if (status.ItemCount[index] < quest.RequiredItemCount[index])
            return false;
    for (unsigned index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
        if (quest.RequiredNpcOrGo[index] && status.CreatureOrGOCount[index] < quest.RequiredNpcOrGoCount[index])
            return false;
    if (status.PlayerCount < quest.GetPlayersSlain()
        || (quest.HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && !status.Explored)
        || (quest.HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) && !status.Timer))
        return false;
    return !quest.GetRepObjectiveFaction()
        || bot.GetReputationMgr().GetReputation(quest.GetRepObjectiveFaction()) >= quest.GetRepObjectiveValue();
}

bool CanPrepareResources(Player const& bot)
{
    return bot.IsAlive() && !bot.IsInCombat() && !bot.IsBeingTeleported() && !bot.IsInFlight()
        && !bot.HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING);
}

bool NeedsEquipmentRepair(Player const& bot)
{
    for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (auto const* item = bot.GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            auto const maximum = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (maximum && uint64_t(item->GetUInt32Value(ITEM_FIELD_DURABILITY)) * 5 <= maximum)
                return true;
        }
    return false;
}

Creature* VisibleRepairer(Player& bot)
{
    auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
    Creature* result = nullptr;
    unsigned scanned = 0;
    if (visible)
        for (auto const& [guid, object] : *visible)
        {
            if (++scanned > 256)
                break;
            auto* creature = object ? object->ToCreature() : nullptr;
            if (!creature || !creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) || !creature->IsAlive()
                || !creature->IsInWorld() || !bot.IsInMap(creature) || !bot.InSamePhase(creature)
                || !bot.CanSeeOrDetect(creature) || !bot.IsFriendlyTo(creature) || !bot.IsWithinLOSInMap(creature)
                || bot.GetExactDist(creature) > 60.0f)
                continue;
            if (!result || bot.GetExactDist(creature) < bot.GetExactDist(result))
                result = creature;
        }
    return result;
}

struct JunkStack
{
    ObjectGuid guid;
    uint32_t count;
    uint32_t price;
};

std::vector<JunkStack> OwnExpendableJunk(Player& bot)
{
    std::set<uint32_t> protectedItems;
    for (auto const quest : QuestLog(bot))
        if (auto const* definition = sObjectMgr->GetQuestTemplate(quest))
        {
            protectedItems.insert(definition->GetSrcItemId());
            for (auto const item : definition->RequiredItemId)
                protectedItems.insert(item);
            for (auto const item : definition->ItemDrop)
                protectedItems.insert(item);
        }
    std::vector<JunkStack> result;
    auto consider = [&](Item const* item)
    {
        if (!item || item->GetOwnerGUID() != bot.GetGUID() || item->IsEquipped() || item->IsInTrade()
            || item->IsRefundable() || item->IsBOPTradable() || item->IsWrapped()
            || bot.GetLootGUID() == item->GetGUID() || protectedItems.contains(item->GetEntry()))
            return;
        auto const* definition = item->GetTemplate();
        if (!definition || definition->Quality != ITEM_QUALITY_POOR || definition->Class != ITEM_CLASS_MISC
            || definition->InventoryType != INVTYPE_NON_EQUIP || definition->StartQuest || !definition->SellPrice
            || item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            return;
        for (auto const& spell : definition->Spells)
            if (spell.SpellId)
                return;
        result.push_back({item->GetGUID(), item->GetCount(), definition->SellPrice});
    };
    for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot.GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8_t slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (auto const* bag = bot.GetBagByPos(slot))
            for (uint32_t item = 0; item < bag->GetBagSize(); ++item)
                consider(bag->GetItemByPos(uint8_t(item)));
    return result;
}

uint32_t FundsWithJunk(Player& bot)
{
    uint32_t funds = bot.GetMoney();
    unsigned stacks = 0;
    for (auto const& item : OwnExpendableJunk(bot))
    {
        if (++stacks > 4)
            break;
        funds += JunkSaleCount(item.count, item.price, funds, MAX_MONEY_AMOUNT - 1, MAX_MONEY_AMOUNT) * item.price;
    }
    return funds; // An affordability estimate; never an observed balance or permission to spend it.
}

void SellPreparationJunk(Player& bot, ObjectGuid vendor, uint32_t desiredMoney, ObjectiveBook& book, uint64_t id)
{
    unsigned attempted = 0;
    for (auto const& candidate : OwnExpendableJunk(bot))
    {
        if (++attempted > 4 || bot.GetMoney() >= desiredMoney || !CanPrepareResources(bot) || !bot.GetSession()
            || CurrentControlMode(bot) != ControlMode::AutonomousSolo)
            break;
        auto const* merchant = bot.GetNPCIfCanInteractWith(vendor, UNIT_NPC_FLAG_VENDOR);
        if (!merchant || merchant->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR))
            break;
        // Resolve inventory and protection again after every ordinary sale and its script callbacks.
        auto const current = OwnExpendableJunk(bot);
        auto const item = std::find_if(current.begin(), current.end(), [&](auto const& value)
        {
            return value.guid == candidate.guid;
        });
        if (item == current.end())
            continue;
        auto const before = bot.GetMoney();
        auto const count = JunkSaleCount(item->count, item->price, before, desiredMoney, MAX_MONEY_AMOUNT);
        if (!count)
            continue;
        WorldPackets::Item::SellItem packet{WorldPacket(CMSG_SELL_ITEM, 20)};
        packet.VendorGuid = vendor;
        packet.ItemGuid = item->guid;
        packet.Count = count;
        bot.GetSession()->HandleSellItemOpcode(packet);
        auto const* afterItem = bot.GetItemByGuid(item->guid);
        auto const afterCount = afterItem ? afterItem->GetCount() : 0;
        if (afterCount != item->count - count || bot.GetMoney() <= before)
            break;
        book.PreparationIncome(id, before, bot.GetMoney());
    }
}

void RepairEquippedItems(Player& bot, ObjectGuid repairer)
{
    if (!bot.GetSession() || !CanPrepareResources(bot)
        || CurrentControlMode(bot) != ControlMode::AutonomousSolo
        || !bot.GetNPCIfCanInteractWith(repairer, UNIT_NPC_FLAG_REPAIR))
        return;
    for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (auto const* item = bot.GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            auto const maximum = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (!maximum || uint64_t(item->GetUInt32Value(ITEM_FIELD_DURABILITY)) * 5 > maximum)
                continue;
            WorldPacket packet(CMSG_REPAIR_ITEM, 17);
            packet << repairer << item->GetGUID() << uint8_t(0);
            bot.GetSession()->HandleRepairItemOpcode(packet);
        }
}

QuestReadiness Readiness(Player& bot, Quest const& quest, QuestObjectiveControl const& control)
{
    QuestReadiness result;
    result.rewarded = bot.GetQuestRewardStatus(quest.GetQuestId());
    result.failed = bot.GetQuestStatus(quest.GetQuestId()) == QUEST_STATUS_FAILED;
    result.turningIn = bot.GetQuestStatus(quest.GetQuestId()) == QUEST_STATUS_COMPLETE;
    auto const money = quest.GetRewOrReqMoney(uint8_t(bot.GetLevel()));
    result.insufficientMoney = money < 0 && uint64_t(bot.GetMoney()) < uint64_t(-int64_t(money));
    // The core marks a quest incomplete again when required money is missing, even after all other credit.
    result.turningIn = result.turningIn || (result.insufficientMoney && HasTurnInCredit(bot, quest));
    result.criticallyDamagedEquipment = NeedsEquipmentRepair(bot);
    result.aboveLevelCapability = bot.GetLevel() + 3 < bot.GetQuestLevel(&quest);
    result.unsupportedExecutor = (quest.GetType() != 0 && quest.GetType() != QUEST_TYPE_ELITE)
        || quest.GetSuggestedPlayers() > 5;
    result.needsCompanions = (quest.GetType() == QUEST_TYPE_ELITE || quest.GetSuggestedPlayers() >= 2)
        && !control.PartyReady(quest.GetQuestId(), quest.GetSuggestedPlayers());
    return result;
}

bool ReadyToAttempt(Player& bot, uint32_t quest, QuestObjectiveControl const& control)
{
    auto const* definition = sObjectMgr->GetQuestTemplate(quest);
    return definition && QuestReadinessObstruction(Readiness(bot, *definition, control)) == Obstruction::None;
}

QuestFinances OwnQuestFinances(Player& bot)
{
    QuestFinances result;
    result.ownMoney = bot.GetMoney();
    for (auto const quest : QuestLog(bot))
        if (auto const* definition = sObjectMgr->GetQuestTemplate(quest))
        {
            result.money.emplace(quest, definition->GetRewOrReqMoney(uint8_t(bot.GetLevel())));
            if (bot.GetQuestStatus(quest) == QUEST_STATUS_COMPLETE || HasTurnInCredit(bot, *definition))
                result.turnInCredit.insert(quest);
        }
    return result;
}

std::string ReadinessReason(QuestReadiness const& readiness)
{
    switch (QuestReadinessObstruction(readiness))
    {
        case Obstruction::Strength: return "My level is below the quest executor's supported range; pursue easier work";
        case Obstruction::Supplies:
            return readiness.turningIn ? "The turn-in requires more money than I currently carry"
                : "Equipped items have at most one fifth durability; repair before attempting more quest combat";
        case Obstruction::Prerequisite: return "The quest is marked failed in my own log; resolve its requirements";
        case Obstruction::Companions: return "This accepted quest calls for an ordinary party; seek willing companions";
        case Obstruction::Executor: return "This quest needs an execution adapter that is not currently available";
        default: return "Own quest conditions permit execution";
    }
}

QuestOpportunity VisibleQuestOpportunity(Player& bot, PlayerbotAI& ai, uint32_t quest)
{
    if (bot.GetQuestStatus(quest) == QUEST_STATUS_COMPLETE)
        return QuestOpportunity::Available; // An outstanding turn-in supersedes waiting for more kill targets.
    auto const& control = ai.rpgInfo.objectiveControl;
    auto const* task = std::get_if<NewRpgInfo::DoQuest>(&ai.rpgInfo.data);
    auto const* definition = sObjectMgr->GetQuestTemplate(quest);
    auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
    if (!definition || !visible || !task || task->questId != quest || !control.Owns(quest)
        || control.phase != QuestObjectiveControl::Phase::Attempting || task->objectiveIdx < 0
        || task->objectiveIdx >= QUEST_OBJECTIVES_COUNT || !bot.IsAlive() || bot.IsBeingTeleported())
        return QuestOpportunity::Unknown;
    int32_t const entry = definition->RequiredNpcOrGo[task->objectiveIdx];
    auto const status = bot.getQuestStatusMap().find(quest);
    if (entry <= 0 || status == bot.getQuestStatusMap().end()
        || status->second.CreatureOrGOCount[task->objectiveIdx] >= definition->RequiredNpcOrGoCount[task->objectiveIdx])
        return QuestOpportunity::Unknown;
    uint32_t available = 0, tagged = 0, corpses = 0, scanned = 0;
    for (auto const& [guid, object] : *visible)
    {
        if (++scanned > 256)
            return ClassifyQuestOpportunity(available, tagged, corpses, false);
        auto const* creature = object ? object->ToCreature() : nullptr;
        if (!creature || creature->GetEntry() != uint32_t(entry) || !creature->IsInWorld()
            || !bot.IsInMap(creature) || !bot.InSamePhase(creature) || !bot.CanSeeOrDetect(creature)
            || bot.GetExactDist(creature) > 60.0f || !bot.IsWithinLOSInMap(creature))
            continue;
        if (!creature->IsAlive())
            ++corpses;
        else if (creature->GetLootRecipientGUID() && !creature->isTappedBy(&bot))
            ++tagged;
        else if (bot.IsValidAttackTarget(creature))
            ++available;
    }
    return ClassifyQuestOpportunity(available, tagged, corpses, true);
}

WorldPosition ResolveArea(Player& bot, uint32_t area, std::vector<WorldPosition> const& excluded = {})
{
    // Executor-only routing to an already validated area. None of these coordinates become personal knowledge.
    WorldPosition result;
    float distance = std::numeric_limits<float>::max();
    auto consider = [&](WorldLocation const& point)
    {
        if (std::any_of(excluded.begin(), excluded.end(), [&point](auto const& failed)
            { return failed.GetMapId() == point.GetMapId() && failed.GetExactDist(point) < 15.0f; }))
            return;
        if (point.GetMapId() != bot.GetMapId() || bot.GetDistance(point) >= distance
            || bot.GetDistance(point) > 5000.0f)
            return;
        if (bot.GetMap()->GetAreaId(bot.GetPhaseMask(), point.GetPositionX(), point.GetPositionY(),
            point.GetPositionZ()) != area)
            return;
        result = point;
        distance = bot.GetDistance(point);
    };
    // Use value-owned caches. The legacy exploration destination list contains borrowed point pointers.
    for (auto const& point : sTravelMgr.GetTravelHubs(&bot))
        consider(point);
    if (result == WorldPosition())
        for (auto const& point : sTravelMgr.GetLocsPerLevelCache(uint8_t(bot.GetLevel())))
            consider(point);
    return result;
}

ObjectiveStep Step(Player const& bot, QuestObjectiveControl const& control)
{
    if (!bot.IsAlive())
        return ObjectiveStep::Recover;
    if (control.cooperationHold)
        return ObjectiveStep::Wait;
    if (bot.IsBeingTeleported() || bot.IsInFlight())
        return ObjectiveStep::Wait;
    if (bot.IsInCombat() && control.phase == QuestObjectiveControl::Phase::Traveling)
        return ObjectiveStep::Wait;
    if (!bot.IsInCombat() && (bot.HasUnitState(UNIT_STATE_STUNNED) || bot.IsSitState()))
        return ObjectiveStep::Wait;
    switch (control.phase)
    {
        case QuestObjectiveControl::Phase::Selecting: return ObjectiveStep::Select;
        case QuestObjectiveControl::Phase::Traveling: return ObjectiveStep::Travel;
        case QuestObjectiveControl::Phase::Attempting: return ObjectiveStep::Attempt;
        case QuestObjectiveControl::Phase::TurnIn: return ObjectiveStep::TurnIn;
    }
    return ObjectiveStep::Wait;
}

boost::json::object Describe(Objective const& objective)
{
    boost::json::array counters;
    for (auto const count : objective.checkpoint.counters)
        counters.push_back(count);
    return {{"id", objective.id}, {"revision", objective.revision}, {"quest", objective.quest},
        {"place", objective.place}, {"plannedMs", objective.plannedMs}, {"arrivedMs", objective.arrivedMs},
        {"discoveredQuest", objective.discoveredQuest}, {"purpose", Name(objective.purpose)},
        {"activityMs", objective.activityMs}, {"completedMs", objective.completedMs},
        {"state", Name(objective.state)}, {"step", Name(objective.step)}, {"reason", objective.reason},
        {"outcome", objective.outcome}, {"approach", objective.approach},
        {"obstruction", Name(objective.obstruction)}, {"attempts", objective.attempts},
        {"credit", std::move(counters)}, {"gainedCredit", objective.gainedCredit},
        {"lastProgressMs", objective.lastProgressMs}, {"activeWithoutProgressMs", objective.activeWithoutProgressMs},
        {"nextReconsiderationMs", objective.nextReconsiderationMs},
        {"preparation", objective.preparation ? boost::json::value(boost::json::object{
            {"capability", objective.preparation->kind == PreparationKind::RepairEquipment
                ? "repair_equipment" : "buy_quest_supplies"}, {"state", Name(objective.preparation->state)},
            {"item", objective.preparation->item}, {"count", objective.preparation->count},
            {"attempts", objective.preparation->attempts}, {"transactions", objective.preparation->transactions},
            {"deadlineMs", objective.preparation->deadlineMs}, {"spentMoney", objective.preparation->spentMoney},
            {"earnedMoney", objective.preparation->earnedMoney},
            {"reconsiderMs", objective.preparation->reconsiderMs}, {"reason", objective.preparation->reason}})
            : boost::json::value(nullptr)},
        {"cooperation", boost::json::object{{"state", Name(objective.cooperation.state)},
            {"quest", objective.cooperation.quest}, {"place", objective.cooperation.rendezvousPlace},
            {"leader", objective.cooperation.leader.id}, {"agreements", objective.cooperation.agreements.size()},
            {"deadlineMs", objective.cooperation.deadlineMs}, {"reason", objective.cooperation.reason}}},
        {"request", objective.request ? boost::json::value(boost::json::object{
            {"source", objective.request->source.name}, {"person", objective.request->source.actor->id},
            {"statement", objective.request->statement}, {"action", objective.approach},
            {"target", objective.request->targetName}, {"acceptedMs", objective.request->acceptedMs},
            {"expiresMs", objective.request->expiresMs}}) : boost::json::value(nullptr)},
        {"information", boost::json::object{{"status", Name(objective.information.status)},
            {"attempts", objective.information.attempts}, {"askedMs", objective.information.askedMs},
            {"expiresMs", objective.information.expiresMs}, {"deliveredMs", objective.information.deliveredMs},
            {"leadReport", objective.information.leadReport}, {"question", objective.information.question}}}};
}
}

Unit* HumanRequestThreat(Player& bot, Player& human)
{
    auto valid = [&](Unit* target)
    {
        return target && target->IsCreature() && !target->IsPet() && !target->GetCharmerOrOwnerGUID()
            && target->IsAlive() && target->IsInMap(&bot) && target->InSamePhase(&bot) && bot.HaveAtClient(target)
            && bot.CanSeeOrDetect(target) && bot.IsValidAttackTarget(target) && bot.GetExactDist(target) <= 40
            && bot.IsWithinLOSInMap(target) && (target->GetVictim() == &human || human.GetVictim() == target);
    };
    if (valid(human.GetVictim()))
        return human.GetVictim();
    for (auto* attacker : human.getAttackers())
        if (valid(attacker))
            return attacker;
    return nullptr;
}

struct ObjectiveRuntime::Impl
{
    struct PendingAdvice
    {
        std::string id;
        AdviceJob job;
        uint64_t expiresRealMs = 0;
        std::optional<Bridge::PlanningResult> result;
    };
    struct PendingDecision
    {
        std::string id;
        ObjectivePlanningJob job;
        uint64_t expiresRealMs = 0;
        std::optional<Bridge::PlanningResult> result;
    };
    struct WaveEmission
    {
        ActorKey owner;
        ActorKey recipient;
        bool delivered = false;
    };
    struct MerchantReceipt
    {
        ActorKey owner;
        ObjectGuid vendor;
        std::optional<MerchantInventory> received;
    };
    struct Owner
    {
        struct SharedQuest
        {
            ActorKey source;
            uint32_t quest = 0;
            uint64_t expiresMs = 0;
        };
        std::optional<SharedQuest> sharedQuest;
        std::optional<PendingDecision> decision;
        uint64_t seenDecisionSignal = 0;
        uint64_t decisionSignalSinceMs = 0;
        uint64_t lastPlannedSignal = 0;
        uint64_t nextPlanningRealMs = 0;
        std::optional<PendingAdvice> advice;
        std::deque<InformationReply> replies;
        ObjectiveBook book;
        PrivateKnowledge knowledge;
        SatisfactionModel satisfaction;
        SatisfactionDecision satisfactionDecision;
        uint64_t satisfactionSampleMs = 0;
        uint64_t nextContactSampleMs = 0;
        uint64_t intentionSinceMs = 0;
        uint64_t intention = 0;
        uint64_t activityObservedMs = 0;
        uint32_t discoveredArea = 0;
        std::map<uint64_t, WorldPosition> destinations;
        std::map<uint64_t, uint64_t> travelTimes;
        std::map<uint64_t, std::vector<WorldPosition>> routes;
        std::vector<WorldPosition> activeRoute;
        std::size_t routeIndex = 0;
        uint64_t nextAssessmentMs = 0;
        uint64_t nextActivityInteractionMs = 0;
        uint64_t activityArrivalObservedMs = 0;
        std::map<uint64_t, double> routeRisks;
        std::map<uint64_t, std::string> routeReasons;
        std::map<uint64_t, uint64_t> emitted;
        uint64_t generation = 0;
        uint64_t attachment = 0;
        uint64_t planningRevision = 0;
        uint32_t currentArea = 0;
        uint32_t partyQuest = 0;
        std::set<uint32_t> observedQuests;
        LocalSurvey survey;
        uint64_t nextDecisionMs = 0;
        uint64_t nextPartyOperationMs = 0;
        uint64_t nextPartyMovementMs = 0;
        WorldPosition partyDestination;
        float nearestPartyDistance = 0;
        uint64_t lastPartyProgressMs = 0;
        uint64_t partyInstance = 0;
        uint64_t partyInstanceObjective = 0;
        std::string notifiedRoster;
        std::set<ActorKey> rosterRecipients;
        WorldPosition routeTarget;
        WorldPosition navigationOrigin;
        std::vector<WorldPosition> failedRoutes;
        float nearestRouteDistance = 0;
        uint64_t lastRouteProgressMs = 0;
        std::optional<ActorKey> followedPerson;
        uint64_t followedObjective = 0;
        ObjectGuid repairer;
        std::optional<RepairLocation> repairRoute;
        float nearestRepairDistance = 0;
        uint64_t lastRepairProgressMs = 0;
        uint64_t nextRepairMovementMs = 0;
        std::optional<MerchantInventory> merchant;
        ObjectGuid lastMerchant;
        uint64_t merchantReceivedMs = 0;
        uint64_t nextMerchantMs = 0;
        std::string availability = "waiting_for_owner";
        boost::json::object bodyStatus;
        std::string bodyEvent;
    };

    Impl(ActorStore& value, std::set<ActorKey> const& owners, Telemetry::Recorder* telemetry,
        ConversationRuntime* dialogue, Bridge::Service* worker, bool planning, bool embodied)
        : store(value), recorder(telemetry), conversation(dialogue), bridge(worker), autonomousPlanning(planning),
          brain(embodied)
    {
        capabilities = ObjectiveCapabilities();
        for (auto const owner : owners)
            states.try_emplace(owner);
    }

    void Release(Player* bot, uint64_t id)
    {
        auto* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        if (!ai || ai->rpgInfo.objectiveControl.token != id)
            return;
        auto const quest = ai->rpgInfo.objectiveControl.quest;
        auto const place = ai->rpgInfo.objectiveControl.place;
        ai->rpgInfo.objectiveControl.Release(id);
        if (ai->rpgInfo.body.Attached())
        {
            auto& body = ai->rpgInfo.body;
            if (body.objective == id && ai->rpgInfo.bodyTravel.HasPath() && Autonomous(*bot, *ai)
                && bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported()
                && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            {
                auto const end = ai->rpgInfo.bodyTravel.End();
                auto const& last = ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
                if (last.lastMoveToMapId == bot->GetMapId() && last.lastMoveToX == end.x
                    && last.lastMoveToY == end.y && last.lastMoveToZ == end.z)
                {
                    bot->GetMotionMaster()->Clear();
                    bot->StopMoving();
                }
            }
            body.Issue(body.generation, body.attachment, 0, BodyControl::Skill::Idle, getMSTime());
            ai->rpgInfo.bodyTravel = {};
        }
        auto const* task = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
        if ((task && task->questId == quest) || (place && (std::holds_alternative<NewRpgInfo::GoCamp>(ai->rpgInfo.data)
            || std::holds_alternative<NewRpgInfo::WanderNpc>(ai->rpgInfo.data))))
        {
            ai->rpgInfo.ChangeToIdle();
            if (Autonomous(*bot, *ai) && ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)
                && bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported())
            {
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
            }
        }
    }

    void ObserveBody(ActorKey owner, Owner& state, PlayerbotAI& ai, uint64_t realMs)
    {
        auto const& body = ai.rpgInfo.body;
        auto const& travel = ai.rpgInfo.bodyTravel;
        auto const end = travel.End();
        state.bodyStatus = {{"attached", true}, {"objective", body.objective},
            {"generation", body.generation}, {"attachment", body.attachment},
            {"skill", std::string(BodyControl::Name(body.skill))},
            {"state", std::string(BodyControl::Name(body.state))},
            {"interruption", std::string(BodyControl::Name(body.interruption))},
            {"route", boost::json::object{{"hasPath", travel.HasPath()}, {"waypoint", travel.Waypoint()},
                {"advances", travel.Advances()}, {"stalledMs", travel.StalledMs()}, {"failures", travel.failures},
                {"x", end.x}, {"y", end.y}, {"z", end.z}}}};
        auto const event = std::to_string(body.objective) + ":" + std::string(BodyControl::Name(body.skill)) + ":"
            + std::string(BodyControl::Name(body.state)) + ":" + std::string(BodyControl::Name(body.interruption));
        if (recorder && state.bodyEvent != event)
        {
            recorder->Record(owner, "alles_body", body.objective, "skill_state",
                boost::json::serialize(state.bodyStatus), realMs);
            state.bodyEvent = event;
        }
    }

    void SyncBody(ActorKey owner, Owner& state, uint64_t realMs)
    {
        if (!brain)
            return;
        auto* bot = Find(owner);
        auto* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        auto const status = store.Status(owner);
        if (!ai || !Autonomous(*bot, *ai) || !ai->rpgInfo.objectiveControl.plannerAttached
            || !status || status->state != ActorState::Ready || state.generation != status->generation
            || state.attachment != status->attachment)
        {
            state.bodyStatus = {{"attached", false}};
            return;
        }
        auto& body = ai->rpgInfo.body;
        auto const now = getMSTime();
        if (!body.Attach(state.generation, state.attachment, now))
        {
            state.availability = "body_owned_by_other_attachment";
            return;
        }
        auto skill = BodyControl::Skill::Idle;
        uint64_t token = 0;
        auto const& control = ai->rpgInfo.objectiveControl;
        if (auto const* following = state.book.Following())
        {
            skill = BodyControl::Skill::Follow;
            token = following->id;
        }
        else if (auto const* preparing = state.book.Preparing())
        {
            skill = preparing->preparation->kind == PreparationKind::RepairEquipment
                ? BodyControl::Skill::Repair : BodyControl::Skill::Supplies;
            token = preparing->id;
        }
        else if (control.cooperationHold)
        {
            token = control.token ? control.token : state.partyInstanceObjective;
            skill = token ? BodyControl::Skill::Rendezvous : BodyControl::Skill::Idle;
        }
        else if (auto const* current = state.book.Current(); current && current->state == ObjectiveState::Active
            && current->id == control.token)
        {
            token = current->id;
            skill = current->purpose != PlacePurpose::Work ? BodyControl::Skill::Activity
                : current->quest ? BodyControl::Skill::Quest
                : std::holds_alternative<NewRpgInfo::GoCamp>(ai->rpgInfo.data)
                    ? BodyControl::Skill::Travel : BodyControl::Skill::Investigate;
        }
        body.Issue(state.generation, state.attachment, token, skill, now);
        ObserveBody(owner, state, *ai, realMs);
    }

    bool Publish(ActorKey owner, Owner& state, uint64_t realMs)
    {
        if (!state.generation)
            return false;
        auto const revision = store.UpdatePlanning(owner, state.generation, state.planningRevision,
            state.book.Capture(), state.knowledge.Capture(), realMs, state.satisfaction.Capture());
        if (!revision)
        {
            state.availability = "planning_save_fenced";
            return false;
        }
        state.planningRevision = *revision;
        for (auto const& [id, objective] : state.book.All())
            if (recorder && state.emitted[id] != objective.revision)
            {
                recorder->Record(owner, "alles_objective", id, objective.reason,
                    boost::json::serialize(Describe(objective)), realMs);
                state.emitted[id] = objective.revision;
            }
        std::erase_if(state.emitted, [&state](auto const& entry) { return !state.book.Find(entry.first); });
        return true;
    }

    bool HumanAvailable(ActorKey owner, uint64_t generation, ActorKey person) const
    {
        auto found = states.find(owner);
        auto const status = store.Status(owner);
        auto* bot = Find(owner);
        auto* human = Find(person);
        auto* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        return owner.kind == ActorKind::Player && person.kind == ActorKind::Player && owner != person
            && found != states.end() && status && status->state == ActorState::Ready
            && generation == status->generation && generation == found->second.generation
            && status->attachment == found->second.attachment && bot && ai && human && human->GetSession()
            && !human->GetSession()->IsBot() && !human->GetSession()->isLogingOut() && !IsSelfBot(human)
            && CurrentControlMode(*bot) == ControlMode::AutonomousSolo
            && ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)
            && !ai->HasStrategy("rpg", BOT_STATE_NON_COMBAT) && !ai->HasStrategy("travel", BOT_STATE_NON_COMBAT)
            && human->IsInWorld() && human->IsAlive() && !human->IsBeingTeleported() && !bot->IsBeingTeleported()
            && bot->IsInMap(human) && bot->IsFriendlyTo(human)
            && (!bot->IsAlive() || (bot->InSamePhase(human) && bot->HaveAtClient(human) && human->HaveAtClient(bot)
                && bot->CanSeeOrDetect(human) && human->CanSeeOrDetect(bot) && bot->GetExactDist(human) <= 100));
    }

    void ReleaseFollow(ActorKey owner, Owner& state)
    {
        auto* bot = Find(owner);
        if (bot && state.followedPerson && CurrentControlMode(*bot) == ControlMode::AutonomousSolo
            && !bot->IsInCombat() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            auto const* movement = static_cast<FollowMovementGenerator<Player>*>(bot->GetMotionMaster()->top());
            if (auto const* target = movement->GetTarget(); target
                && target->GetGUID() == ObjectGuid(HighGuid::Player, uint32_t(state.followedPerson->id)))
            {
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
            }
        }
        state.followedPerson.reset();
        state.followedObjective = 0;
    }

    bool FollowRequest(ActorKey owner, Owner& state, Player& bot, PlayerbotAI& ai, uint64_t now, uint64_t realMs)
    {
        auto const* objective = state.book.Following();
        if (!objective)
        {
            ReleaseFollow(owner, state);
            return false;
        }
        auto const id = objective->id;
        auto const person = *objective->person;
        auto* human = Find(person);
        bool const available = HumanAvailable(owner, state.generation, person);
        state.book.ObserveRequest(id,
            {available, !bot.IsAlive(), available && bot.GetExactDist(human) <= 6, false}, now);
        if (!state.book.Following())
        {
            ReleaseFollow(owner, state);
            Publish(owner, state, realMs);
            return false;
        }
        auto& control = ai.rpgInfo.objectiveControl;
        control.plannerAttached = true;
        control.cooperationHold = true;
        state.availability = "following_human_request";
        if (!Publish(owner, state, realMs))
            return true;
        if (bot.IsAlive() && !bot.IsInCombat() && !bot.IsInFlight() && !bot.IsNonMeleeSpellCast(false)
            && !bot.GetLootGUID())
        {
            bool matching = false;
            if (bot.GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            {
                auto const* movement = static_cast<FollowMovementGenerator<Player>*>(bot.GetMotionMaster()->top());
                matching = movement->GetTarget() == human;
            }
            if (!matching)
                bot.GetMotionMaster()->MoveFollow(human, 3.0f, float(owner.id % 6));
            state.followedPerson = person;
            state.followedObjective = id;
        }
        return true;
    }

    boost::json::object HumanContext(ActorKey owner, uint64_t generation, ActorKey person) const
    {
        boost::json::object context{{"localActions", false}, {"canFollow", false},
            {"followingPlayer", false}, {"nearbyThreat", ""}};
        if (!HumanAvailable(owner, generation, person))
            return context;
        auto& bot = *Find(owner);
        auto& human = *Find(person);
        if (!bot.IsAlive() || bot.GetExactDist(&human) > 40 || !bot.IsWithinLOSInMap(&human))
            return context;
        auto const& book = states.at(owner).book;
        auto const* following = book.Following();
        bool const committed = std::any_of(book.All().begin(), book.All().end(), [](auto const& item)
        {
            auto const phase = item.second.cooperation.state;
            return phase > CooperationState::None && phase < CooperationState::Completed;
        });
        context["localActions"] = true;
        context["followingPlayer"] = following && following->person == person;
        context["canFollow"] = !bot.IsInCombat() && !committed && (!following || following->person == person);
        if (auto const* threat = HumanRequestThreat(bot, human); threat && !bot.IsInCombat())
            context["nearbyThreat"] = threat->GetName();
        return context;
    }

    std::string ApplyHumanRequest(ActorKey owner, uint64_t generation, Reference const& source,
        std::string const& statement, std::string const& action, ObjectGuid const& threat,
        uint64_t now, uint64_t realMs)
    {
        if (!source.actor || !IsSafeChatText(statement) || !HumanAvailable(owner, generation, *source.actor)
            || source.name != Find(*source.actor)->GetName())
            return "I can't act on that request right now.";
        auto const context = HumanContext(owner, generation, *source.actor);
        if (!context.at("localActions").as_bool())
            return "Please stay nearby so I can help.";
        auto& state = states.at(owner);
        auto& bot = *Find(owner);
        auto& human = *Find(*source.actor);
        auto* ai = sPlayerbotsMgr.GetPlayerbotAI(&bot);
        auto permitted = [&](Objective const& objective)
        {
            CapabilityContext current{owner, generation, objective.id, objective.revision,
                true, {}, {}, {*source.actor}};
            CapabilityRequest request{1, owner, generation, objective.id, objective.revision,
                action, 0, 0, source.actor};
            return capabilities.Validate(request, current).empty();
        };
        if (action == "stop")
        {
            auto const* following = state.book.Following();
            if (!following || following->person != source.actor || !permitted(*following))
                return "I'm not accompanying you at the moment.";
            if (!state.book.Cancel(following->id, "The requesting player asked me to stop: " + statement)
                || !Publish(owner, state, realMs))
                return "I couldn't finish that request right now.";
            ReleaseFollow(owner, state);
            ai->rpgInfo.objectiveControl.cooperationHold = false;
            return {};
        }
        if ((action == "follow" && !context.at("canFollow").as_bool())
            || (action != "follow" && action != "wave" && action != "assist")
            || now > std::numeric_limits<uint64_t>::max() - 120000)
            return "I can't commit to that action right now.";
        auto* target = action == "assist" ? HumanRequestThreat(bot, human) : nullptr;
        if (action == "assist" && (!target || target->GetGUID() != threat || bot.IsInCombat()))
            return "I can't engage that threat right now.";
        auto book = state.book;
        auto const* previous = state.book.Current();
        auto const release = previous ? previous->id : 0;
        RequestAction const kind = action == "follow" ? RequestAction::Follow
            : action == "wave" ? RequestAction::Wave : RequestAction::Assist;
        auto const id = book.Request({kind, source, statement, target ? target->GetName() : "", now,
            now + (kind == RequestAction::Follow ? 120000 : 30000)});
        if (!id || !permitted(*book.Find(*id)))
            return "My current commitments prevent that request.";
        std::swap(book, state.book);
        if (!Publish(owner, state, realMs))
        {
            std::swap(book, state.book);
            return "I couldn't retain that request right now.";
        }
        if (kind == RequestAction::Follow)
        {
            if (release)
                Release(&bot, release);
            if (bridge && state.decision)
                bridge->CancelPlanning(state.decision->id);
            if (bridge && state.advice)
                bridge->CancelPlanning(state.advice->id);
            state.decision.reset();
            state.advice.reset();
            FollowRequest(owner, state, bot, *ai, now, realMs);
            return {};
        }
        bool effect = false;
        if (kind == RequestAction::Wave)
        {
            wave = WaveEmission{owner, *source.actor};
            struct ClearWave
            {
                std::optional<WaveEmission>& value;
                ~ClearWave() { value.reset(); }
            } clear{wave};
            bot.HandleEmoteCommand(EMOTE_ONESHOT_WAVE);
            effect = wave->delivered;
        }
        else
        {
            CooperativeAssist(ai).Engage(target);
            effect = bot.IsInCombat() && bot.GetVictim() == target;
        }
        state.book.ObserveRequest(*id, {true, false, false, effect}, now);
        if (!effect)
            state.book.Cancel(*id, "The requested effect was not observed");
        Publish(owner, state, realMs);
        if (recorder)
            recorder->Record(owner, "alles_human_request", *id, effect ? "effect_observed" : "effect_unobserved",
                action, realMs);
        return effect ? "" : "I couldn't carry out that action just now.";
    }

    void Coordinate(ActorKey owner, Owner& state, Player& bot, PlayerbotAI& ai,
        Cooperation& cooperation, PartyObservation const& observation, uint64_t now, uint64_t realMs)
    {
        if (cooperation.state == CooperationState::None || cooperation.state >= CooperationState::Completed)
            return;
        bool const working = cooperation.state == CooperationState::Working;
        bool const ready = working && observation.members.size() >= observation.requiredMembers
            && std::all_of(observation.members.begin(), observation.members.end(), [](auto const& member)
                { return member.online && member.alive && member.eligible && member.ready && member.inRendezvous; });
        auto const guide = working ? CooperativeGuide(cooperation, observation)
            : std::optional<ActorKey>(cooperation.leader);
        auto& control = ai.rpgInfo.objectiveControl;
        control.cooperativeTurnIn = ready && std::all_of(observation.members.begin(), observation.members.end(),
            [](auto const& member) { return member.finished || member.readyToReward; });
        control.cooperationHold = !ready || guide != owner;
        if (ready)
        {
            state.partyQuest = cooperation.quest;
            control.cooperativeQuest = cooperation.quest;
            control.partyMembers = uint8_t(observation.members.size());
        }
        if (!guide || !bot.IsAlive() || bot.IsInCombat() || bot.IsBeingTeleported() || bot.IsInFlight()
            || bot.IsNonMeleeSpellCast(false) || bot.GetLootGUID() || now < state.nextPartyMovementMs)
            return;
        state.nextPartyMovementMs = now + 2000;
        auto* peer = *guide == owner ? &bot : FindCooperativePeer(bot, cooperation, *guide);
        if (peer && peer != &bot && ready && peer->IsInCombat() && bot.HaveAtClient(peer)
            && bot.CanSeeOrDetect(peer) && bot.GetExactDist(peer) < 40.0f)
        {
            auto* threat = peer->GetVictim();
            if (threat && threat->IsCreature() && !threat->IsPet() && !threat->GetCharmerOrOwnerGUID()
                && threat->IsAlive() && bot.HaveAtClient(threat) && bot.CanSeeOrDetect(threat)
                && bot.IsValidAttackTarget(threat) && bot.IsWithinLOSInMap(threat)
                && bot.GetExactDist(threat) < 40.0f)
            {
                CooperativeAssist(&ai).Engage(threat);
                return;
            }
        }
        WorldPosition destination;
        if (peer && peer != &bot && (working || peer->GetAreaId() == cooperation.rendezvousPlace))
        {
            // Ordinary party updates disclose member map positions. These stay executor-only, never worker knowledge.
            if (bot.GetExactDist(peer) <= 12.0f)
            {
                state.partyDestination = WorldPosition();
                return;
            }
            destination = WorldPosition(peer);
        }
        else if (!working && bot.GetAreaId() != cooperation.rendezvousPlace
            && state.knowledge.Places().contains(cooperation.rendezvousPlace))
            destination = ResolveArea(bot, cooperation.rendezvousPlace);
        if (destination == WorldPosition() || destination.GetMapId() != bot.GetMapId()
            || bot.GetDistance(destination) > 5000.0f)
            return; // Missing routes expire through the already bounded agreement; they do not authorize teleport.
        float const distance = bot.GetDistance(destination);
        if (state.partyDestination == WorldPosition() || state.partyDestination.GetMapId() != destination.GetMapId()
            || state.partyDestination.GetExactDist(destination) > 30.0f)
        {
            state.partyDestination = destination;
            state.nearestPartyDistance = distance;
            state.lastPartyProgressMs = now;
        }
        if (distance + 5.0f < state.nearestPartyDistance)
        {
            state.nearestPartyDistance = distance;
            state.lastPartyProgressMs = now;
        }
        else if (now >= state.lastPartyProgressMs && now - state.lastPartyProgressMs >= 90000)
        {
            DeferCooperation(cooperation, "No measured advancement toward the agreed party for 90 seconds", now);
            state.partyQuest = 0;
            control.cooperativeQuest = 0;
            control.partyMembers = 0;
            control.cooperationHold = true;
            return;
        }
        if (CooperativeMovement(&ai).Walk(destination) && recorder)
            recorder->Record(owner, "alles_cooperation", cooperation.objective, "rendezvous_movement_attempted",
                "Ordinary movement toward the agreed party; arrival remains an observation", realMs);
    }

    std::optional<QuestProgress> HelpEligibility(ActorKey owner, uint64_t generation,
        RecruitmentNotice const& notice, uint64_t now, uint64_t realMs) const
    {
        auto found = states.find(owner);
        auto const status = store.Status(owner);
        auto* bot = Find(owner);
        auto* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        auto const* quest = sObjectMgr->GetQuestTemplate(notice.question.topic.quest);
        if (found == states.end() || !status || status->state != ActorState::Ready
            || generation != status->generation || generation != found->second.generation
            || status->attachment != found->second.attachment || !conversation
            || !conversation->RecruitmentActive(notice, realMs) || !bot || !ai || !quest
            || CurrentControlMode(*bot) != ControlMode::AutonomousSolo || !ai->rpgInfo.objectiveControl.plannerAttached
            || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsInFlight()
            || bot->GetHealthPct() < 50.0f || bot->GetLevel() + 3 < bot->GetQuestLevel(quest)
            || (quest->GetType() != 0 && quest->GetType() != QUEST_TYPE_ELITE)
            || quest->GetSuggestedPlayers() > 5)
            return std::nullopt;
        auto progress = Sample(*bot, quest->GetQuestId());
        if (!CanOfferHelp(owner, found->second.book, notice, progress, bot->CanTakeQuest(quest, false), now))
            return std::nullopt;
        return progress;
    }

    bool OfferHelp(ActorKey owner, uint64_t generation, RecruitmentNotice const& notice,
        std::string const& statement, uint64_t now, uint64_t realMs)
    {
        auto progress = HelpEligibility(owner, generation, notice, now, realMs);
        if (!progress || !IsSafeChatText(statement))
            return false;
        auto& state = states.at(owner);
        auto* bot = Find(owner);
        auto const* current = state.book.Current();
        auto const release = current ? current->id : 0;
        auto const intention = RecordHelpOffer(owner, bot->GetName(), state.book, state.knowledge,
            notice, *progress, true, statement, now);
        if (!intention || !Publish(owner, state, realMs))
            return false;
        if (release)
            Release(bot, release);
        if (bridge && state.decision)
            bridge->CancelPlanning(state.decision->id);
        if (bridge && state.advice)
            bridge->CancelPlanning(state.advice->id);
        state.decision.reset();
        state.advice.reset();
        state.replies.clear();
        if (recorder)
            recorder->Record(owner, "alles_cooperation", *intention, "help_offered",
                "Own agreement recorded; delivery, invitation and actual membership are still required", realMs);
        return true;
    }

    bool ReceiveRoster(ActorKey owner, uint64_t generation, CooperativeRoster const& roster,
        Reference const& source, uint64_t now, uint64_t realMs)
    {
        auto found = states.find(owner);
        auto const status = store.Status(owner);
        auto* bot = Find(owner);
        if (found == states.end() || !status || status->state != ActorState::Ready || !bot
            || generation != status->generation || generation != found->second.generation
            || status->attachment != found->second.attachment || source.actor != roster.leader
            || (CurrentControlMode(*bot) != ControlMode::AutonomousSolo
                && CurrentControlMode(*bot) != ControlMode::AutonomousParty))
            return false;
        auto& state = found->second;
        for (auto const& [id, objective] : state.book.All())
        {
            auto cooperation = objective.cooperation;
            if (cooperation.leader != roster.leader || cooperation.quest != roster.quest
                || cooperation.rendezvousPlace != roster.place
                || !AdoptCooperativeRoster(cooperation, source, roster.members, roster.text, now))
                continue;
            auto book = state.book;
            auto knowledge = state.knowledge;
            if (!knowledge.Hear(source, {Activity::Companions, roster.quest, roster.place, roster.leader},
                roster.text, now, 0.4) || !book.SetCooperation(id, objective.revision, std::move(cooperation)))
                return false;
            state.book = std::move(book);
            state.knowledge = std::move(knowledge);
            return Publish(owner, state, realMs);
        }
        return false;
    }

    void RecruitParty(ActorKey owner, Owner& state, Player& bot, Cooperation const& cooperation,
        uint64_t now, uint64_t realMs)
    {
        auto const* quest = sObjectMgr->GetQuestTemplate(cooperation.quest);
        if (!conversation || !quest || cooperation.leader != owner
            || bot.GetQuestStatus(cooperation.quest) != QUEST_STATUS_INCOMPLETE
            || cooperation.agreements.size() < std::max<uint32_t>(2, quest->GetSuggestedPlayers())
            || !Publish(owner, state, realMs))
            return;
        CooperativeRoster roster{owner, cooperation.quest, cooperation.rendezvousPlace};
        roster.text = "Our agreed party for \"" + quest->GetTitle() + "\": ";
        for (auto const& [actor, agreement] : cooperation.agreements)
        {
            if (!roster.members.empty())
                roster.text += ", ";
            roster.members.push_back(agreement.person);
            roster.text += agreement.person.name;
        }
        roster.text += ".";
        if (!IsSafeChatText(roster.text))
            return;
        auto const key = std::to_string(cooperation.objective) + ":" + std::to_string(cooperation.startedMs)
            + ":" + std::to_string(cooperation.rendezvousPlace) + ":" + roster.text;
        if (state.notifiedRoster != key)
        {
            state.notifiedRoster = key;
            state.rosterRecipients.clear();
        }
        for (auto const& [actor, agreement] : cooperation.agreements)
            if (actor != owner && !state.rosterRecipients.contains(actor)
                && conversation->SendRoster(owner, actor, roster, now, realMs))
                state.rosterRecipients.insert(actor);
        if (state.rosterRecipients.size() + 1 != cooperation.agreements.size())
            return; // Every member hears the roster before a new member can make the existing party appear unagreed.
        auto const present = ObserveParty(bot, cooperation);
        for (auto const& [actor, agreement] : cooperation.agreements)
        {
            if (actor == owner || std::any_of(present.members.begin(), present.members.end(),
                [&](auto const& member) { return member.actor == actor; }))
                continue;
            if (auto* peer = Find(actor))
            {
                bool const invited = InviteCompanion(bot, *peer, cooperation, now);
                if (recorder)
                    recorder->Record(owner, "alles_cooperation", cooperation.objective, "invitation_attempted",
                        Acore::StringFormat("recipient={} invitationObserved={}", actor.id, invited), realMs);
            }
        }
    }

    void ProcessAdvice(ActorKey owner, Owner& state, uint64_t now, uint64_t realMs)
    {
        if (!bridge || !conversation)
            return;
        for (auto& reply : conversation->TakeInformationReplies(owner))
            if (state.replies.size() < 8)
                state.replies.push_back(std::move(reply));
        if (state.advice)
        {
            auto& pending = *state.advice;
            auto const* objective = state.book.Find(pending.job.issued.objective);
            bool const stale = !objective || objective->revision != pending.job.issued.revision
                || state.generation != pending.job.issued.actorGeneration || realMs >= pending.expiresRealMs;
            if (stale || pending.result)
            {
                AdviceOutcome outcome{"worker_expired_or_stale"};
                if (!stale && pending.result->status == "success")
                {
                    auto live = pending.job.issued;
                    live.revision = objective->revision;
                    live.actorGeneration = state.generation;
                    try
                    {
                        auto decision = Bridge::DecodePlanningDecision(pending.result->response, pending.job.issued);
                        outcome = ApplyAdvice(pending.job, decision, live, state.book, state.knowledge, now);
                    }
                    catch (std::exception const&)
                    {
                        outcome.status = "invalid_planning_result";
                    }
                }
                else if (!stale)
                    outcome.status = "worker_" + pending.result->status;
                bool const agreementPublished = outcome.invite && Publish(owner, state, realMs);
                if (recorder)
                    recorder->Record(owner, "alles_advice", pending.job.issued.objective, outcome.status,
                        boost::json::serialize(boost::json::object{{"job", pending.id}, {"report", outcome.report},
                            {"intention", outcome.intention}, {"source", pending.job.reply.source.name},
                            {"receivedMs", pending.job.reply.gameMs}, {"agreementPublished", agreementPublished}}),
                        realMs);
                bridge->CancelPlanning(pending.id);
                state.advice.reset();
            }
        }
        if (state.advice || state.decision)
            return;
        while (!state.replies.empty())
        {
            auto reply = std::move(state.replies.front());
            state.replies.pop_front();
            if (realMs < reply.realMs || realMs - reply.realMs > 120000)
                continue;
            // Resolve only names literally heard. The dictionary is never sent to the worker or retained privately.
            std::map<uint32_t, std::string> dictionary;
            for (uint32_t index = 0; index < sAreaTableStore.GetNumRows(); ++index)
                if (auto const* area = sAreaTableStore.LookupEntry(index))
                    dictionary.emplace(area->ID, PlayerbotAI::GetLocalizedAreaName(area));
            for (auto const& [id, place] : state.knowledge.Places())
                dictionary[id] = place.name;
            auto job = PrepareAdvice(owner, state.generation, state.book, state.knowledge,
                std::move(reply), dictionary, now);
            if (!job)
                continue;
            auto id = "advice-" + std::to_string(owner.id) + "-" + std::to_string(++nextAdviceId);
            if (bridge->QueuePlanning(id, job->context, realMs))
            {
                state.advice = PendingAdvice{std::move(id), std::move(*job), realMs + 45000, {}};
                break;
            }
        }
    }

    void AssessReports(Owner& state, uint32_t area, bool useful)
    {
        for (auto const& [id, report] : state.knowledge.Reports())
            if (report.topic.place == area)
                state.knowledge.Assess(id, area, useful);
    }

    void AskForInformation(ActorKey owner, Owner& state, Player& bot, uint64_t now, uint64_t realMs,
        uint64_t requested = 0)
    {
        auto const* current = state.book.Current();
        if (!conversation || bot.IsInCombat() || (current && current->quest) || !conversation->CanAsk(owner))
            return;
        for (auto it = state.book.All().rbegin(); it != state.book.All().rend(); ++it)
        {
            auto const& objective = it->second;
            if ((requested && requested != objective.id) || !state.book.CanAsk(objective.id, now))
                continue;
            std::string text;
            std::string questName, placeName;
            uint8_t partySize = 2;
            bool const recruiting = objective.obstruction == Obstruction::Companions;
            if (objective.quest)
            {
                auto const* quest = sObjectMgr->GetQuestTemplate(objective.quest);
                if (!quest || bot.FindQuestSlot(objective.quest) >= MAX_QUEST_LOG_SIZE)
                    continue;
                if (recruiting)
                {
                    auto const place = state.knowledge.Places().find(state.currentArea);
                    if (CurrentControlMode(bot) != ControlMode::AutonomousSolo
                        || place == state.knowledge.Places().end() || !IsBoundedText(quest->GetTitle(), 100))
                        continue;
                    questName = quest->GetTitle();
                    placeName = place->second.name;
                    partySize = uint8_t(std::clamp<uint32_t>(quest->GetSuggestedPlayers(), 2, 5));
                    text = "I'm seeking " + std::to_string(partySize - 1) + " companions for \"" + questName
                        + "\". Let's meet in " + placeName + ".";
                }
                else
                    text = "Does anyone have advice for \"" + quest->GetTitle() + "\"? I'm stuck, around level "
                        + std::to_string(bot.GetLevel()) + ".";
            }
            else
            {
                auto place = state.knowledge.Places().find(objective.place);
                if (place == state.knowledge.Places().end())
                    continue;
                text = "I've searched " + place->second.name
                    + " without finding work. Where else could someone around level "
                    + std::to_string(bot.GetLevel()) + " look?";
            }
            if (recruiting && !IsSafeChatText(text))
                continue;
            if (!IsSafeChatText(text))
                text = "I'm having trouble finding useful work around level "
                    + std::to_string(bot.GetLevel()) + ". Does anyone have a suggestion?";
            if (recruiting)
            {
                auto cooperation = objective.cooperation;
                if (!BeginRecruitment(cooperation, owner, objective.id, objective.quest, state.currentArea,
                    {{owner, bot.GetName()}, text, now}, now)
                    || !state.book.SetCooperation(objective.id, objective.revision, std::move(cooperation)))
                    continue;
            }
            if (!state.book.Ask(objective.id, objective.revision, text, now) || !Publish(owner, state, realMs))
                return;
            // The semantic attempt is in the owner snapshot before any emitted packet or reentrant reply.
            InformationQuestion question{owner, state.generation, objective.id, objective.revision,
                objective.information.attempts,
                {recruiting ? Activity::Companions : Activity::Work, objective.quest,
                    recruiting ? objective.cooperation.rendezvousPlace : objective.place}, text, questName, placeName,
                partySize};
            bool const delivered = conversation->Ask(question, now, realMs);
            state.book.QuestionDelivery(objective.id, question.attempt, delivered, now);
            return;
        }
    }

    std::optional<MerchantInventory> ReadMerchant(ActorKey owner, Player& bot, ObjectGuid vendor)
    {
        if (merchantReceipt || !CanPrepareResources(bot)
            || CurrentControlMode(bot) != ControlMode::AutonomousSolo || !bot.GetSession()
            || !bot.GetNPCIfCanInteractWith(vendor, UNIT_NPC_FLAG_VENDOR))
            return std::nullopt;
        merchantReceipt = MerchantReceipt{owner, vendor, {}};
        struct Reset
        {
            std::optional<MerchantReceipt>& receipt;
            ~Reset() { receipt.reset(); }
        } reset{merchantReceipt};
        WorldPackets::Item::ListInventory packet{WorldPacket(CMSG_LIST_INVENTORY, 8)};
        packet.VendorGuid = vendor;
        bot.GetSession()->HandleListInventoryOpcode(packet);
        return merchantReceipt->received;
    }

    uint32_t ReservedQuestMoney(Player& bot) const
    {
        uint64_t reserved = 0;
        for (auto const quest : QuestLog(bot))
            if (auto const* definition = sObjectMgr->GetQuestTemplate(quest))
            {
                auto const money = definition->GetRewOrReqMoney(uint8_t(bot.GetLevel()));
                if (money < 0 && (bot.GetQuestStatus(quest) == QUEST_STATUS_COMPLETE
                    || HasTurnInCredit(bot, *definition)))
                    reserved += uint64_t(-int64_t(money));
            }
        return uint32_t(std::min<uint64_t>(reserved, std::numeric_limits<uint32_t>::max()));
    }

    std::optional<MerchantPurchase> SupplyPurchase(Player& bot, Objective const& objective,
        MerchantInventory const& menu, bool includeJunk = false) const
    {
        if (!objective.preparation || objective.preparation->kind != PreparationKind::BuyQuestSupplies
            || bot.FindQuestSlot(objective.quest) >= MAX_QUEST_LOG_SIZE)
            return std::nullopt;
        auto const* definition = sObjectMgr->GetQuestTemplate(objective.quest);
        auto const& preparation = *objective.preparation;
        bool required = false;
        if (definition)
            for (unsigned index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
                required = required || (definition->RequiredItemId[index] == preparation.item
                    && definition->RequiredItemCount[index] == preparation.count);
        auto const ownCount = bot.GetItemCount(preparation.item, false);
        if (!required || ownCount >= preparation.count)
            return std::nullopt;
        auto const* vendor = bot.GetNPCIfCanInteractWith(ObjectGuid(menu.vendor), UNIT_NPC_FLAG_VENDOR);
        uint32_t const funds = includeJunk && vendor && !vendor->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR)
            ? FundsWithJunk(bot) : bot.GetMoney();
        std::optional<MerchantPurchase> best;
        for (auto const& offer : menu.offers)
            if (offer.item == preparation.item)
                if (auto purchase = AffordablePurchase(offer, preparation.count - ownCount,
                    funds, ReservedQuestMoney(bot)); purchase
                    && (!best || purchase->maximumPrice < best->maximumPrice))
                    best = purchase;
        return best;
    }

    ObjectGuid VisibleMerchant(Player& bot, ObjectGuid previous = ObjectGuid::Empty) const
    {
        auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
        unsigned scanned = 0;
        ObjectGuid fallback;
        if (visible)
            for (auto const& [guid, object] : *visible)
            {
                if (++scanned > 256)
                    break;
                auto const* creature = object ? object->ToCreature() : nullptr;
                if (creature && bot.GetNPCIfCanInteractWith(creature->GetGUID(), UNIT_NPC_FLAG_VENDOR))
                {
                    if (creature->GetGUID() != previous)
                        return creature->GetGUID();
                    fallback = previous;
                }
            }
        return fallback;
    }

    void RefreshMerchant(ActorKey owner, Owner& state, Player& bot, uint64_t now)
    {
        if (!CanPrepareResources(bot)
            || NeedsEquipmentRepair(bot) || CurrentControlMode(bot) != ControlMode::AutonomousSolo
            || state.book.Following() || state.book.Preparing())
            return;
        bool hasNeed = false;
        for (auto const& [id, objective] : state.book.All())
            if (objective.checkpoint.inLog && !objective.checkpoint.failed && !objective.checkpoint.readyToReward)
                if (auto const* definition = sObjectMgr->GetQuestTemplate(objective.quest))
                    for (unsigned index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
                        hasNeed = hasNeed || (definition->RequiredItemId[index]
                            && bot.GetItemCount(definition->RequiredItemId[index], false)
                                < definition->RequiredItemCount[index]);
        if (!hasNeed)
            return;
        if (state.merchant && (now < state.merchantReceivedMs || now - state.merchantReceivedMs >= 30000
            || !bot.GetNPCIfCanInteractWith(ObjectGuid(state.merchant->vendor), UNIT_NPC_FLAG_VENDOR)))
            state.merchant.reset();
        if (!state.merchant && now >= state.nextMerchantMs)
        {
            state.nextMerchantMs = now + 1000;
            if (auto const vendor = VisibleMerchant(bot, state.lastMerchant))
            {
                state.nextMerchantMs = now + 30000;
                state.lastMerchant = vendor;
                state.merchant = ReadMerchant(owner, bot, vendor);
                state.merchantReceivedMs = now;
            }
        }
        if (!state.merchant)
            return;
        for (auto const& [id, objective] : state.book.All())
        {
            auto const* definition = objective.quest ? sObjectMgr->GetQuestTemplate(objective.quest) : nullptr;
            if (!definition || !objective.checkpoint.inLog || objective.checkpoint.failed
                || objective.checkpoint.rewarded || objective.checkpoint.readyToReward
                || (objective.preparation && objective.preparation->state != PreparationState::Completed))
                continue;
            for (unsigned index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
            {
                auto const item = definition->RequiredItemId[index];
                auto const count = definition->RequiredItemCount[index];
                if (!item || bot.GetItemCount(item, false) >= count)
                    continue;
                auto candidate = objective;
                candidate.preparation = ResourcePreparation{};
                candidate.preparation->kind = PreparationKind::BuyQuestSupplies;
                candidate.preparation->item = item;
                candidate.preparation->count = count;
                if (SupplyPurchase(bot, candidate, *state.merchant, true))
                {
                    state.book.ProposeSupplies(id, item, count);
                    state.book.ObservePreparationFunds(id, bot.GetMoney(), now);
                    break;
                }
            }
        }
    }

    bool PrepareSupplies(ActorKey owner, Owner& state, Player& bot, PlayerbotAI& ai, uint64_t now, uint64_t realMs)
    {
        auto& book = state.book;
        auto const* objective = book.Preparing();
        if (objective && objective->preparation->kind != PreparationKind::BuyQuestSupplies)
            return false;
        if (!objective && state.merchant && now >= state.merchantReceivedMs
            && now - state.merchantReceivedMs < 30000 && CanPrepareResources(bot) && !NeedsEquipmentRepair(bot))
            for (auto const& [id, candidate] : book.All())
                if ((!brain || state.satisfactionDecision.selected == id)
                    && book.CanBuySupplies(id, now) && SupplyPurchase(bot, candidate, *state.merchant, true))
                {
                    CapabilityContext context{owner, state.generation, id, candidate.revision,
                        true, {candidate.quest}, {}, {}};
                    CapabilityRequest request{1, owner, state.generation, id, candidate.revision,
                        "buy_quest_supplies", candidate.quest, 0, {}};
                    if (capabilities.Validate(request, context).empty()
                        && book.BeginSupplyPurchase(id, candidate.revision, now))
                    {
                        objective = book.Preparing();
                        break;
                    }
                }
        if (!objective)
            return false;
        auto const id = objective->id;
        auto& control = ai.rpgInfo.objectiveControl;
        if (control.token)
            Release(&bot, control.token);
        control.cooperationHold = true;
        ReleaseRepairMovement(owner, state);
        book.ObserveSupplies(id, bot.GetItemCount(objective->preparation->item, false), now);
        if (book.Preparing() && CanPrepareResources(bot))
        {
            // Re-open the ordinary menu immediately before buying; cached prices/stock are not transaction authority.
            auto const vendor = state.merchant ? ObjectGuid(state.merchant->vendor) : VisibleMerchant(bot);
            auto menu = vendor ? ReadMerchant(owner, bot, vendor) : std::nullopt;
            auto purchase = menu ? SupplyPurchase(bot, *objective, *menu, true) : std::nullopt;
            if (purchase && book.PreparationTransaction(id, now))
            {
                if (!Publish(owner, state, realMs))
                    return true;
                auto const desiredMoney = uint32_t(ReservedQuestMoney(bot) + purchase->maximumPrice);
                SellPreparationJunk(bot, vendor, desiredMoney, book, id);
                // Actual proceeds may differ or a script may reject a sale. Only current funds can buy anything.
                menu = ReadMerchant(owner, bot, vendor);
                purchase = menu ? SupplyPurchase(bot, *objective, *menu) : std::nullopt;
                if (!purchase)
                {
                    book.DeferPreparation(id, "Current own funds or merchant stock cannot supply the required items",
                        now);
                    control.cooperationHold = false;
                    Publish(owner, state, realMs);
                    return false;
                }
                auto const before = bot.GetMoney();
                WorldPackets::Item::BuyItem packet{WorldPacket(CMSG_BUY_ITEM, 21)};
                packet.VendorGuid = ObjectGuid(menu->vendor);
                packet.Item = purchase->item;
                packet.Slot = purchase->slot;
                packet.Count = purchase->bundles;
                bot.GetSession()->HandleBuyItemOpcode(packet);
                book.PreparationExpense(id, before, bot.GetMoney());
                book.ObserveSupplies(id, bot.GetItemCount(purchase->item, false), now);
                book.Observe(id, Sample(bot, objective->quest), ObjectiveStep::Wait, now);
            }
            if (book.Preparing())
                book.DeferPreparation(id, "No required items acquired from the current affordable merchant offer", now);
        }
        state.availability = "preparing_quest_supplies";
        control.cooperationHold = book.Preparing() != nullptr;
        Publish(owner, state, realMs);
        return book.Preparing() != nullptr;
    }

    void ReleaseRepairMovement(ActorKey owner, Owner& state)
    {
        auto* bot = Find(owner);
        if (bot && state.repairRoute && CurrentControlMode(*bot) == ControlMode::AutonomousSolo
            && !bot->IsInCombat() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            float x = 0, y = 0, z = 0;
            if (bot->GetMotionMaster()->GetDestination(x, y, z)
                && x == state.repairRoute->x && y == state.repairRoute->y && z == state.repairRoute->z)
            {
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
            }
        }
        state.repairRoute.reset();
        if (bot && state.repairer && CurrentControlMode(*bot) == ControlMode::AutonomousSolo
            && !bot->IsInCombat() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            auto const* movement = static_cast<FollowMovementGenerator<Player>*>(bot->GetMotionMaster()->top());
            if (auto const* target = movement->GetTarget(); target && target->GetGUID() == state.repairer)
            {
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
            }
        }
        state.repairer.Clear();
    }

    bool ReturnToKnownRepair(ActorKey owner, Owner& state, Player& bot, uint64_t now, uint64_t realMs)
    {
        if (bot.GetMap()->Instanceable())
            return false;
        if (!state.repairRoute)
        {
            auto const* place = state.knowledge.NearestRepair(bot.GetMapId(), bot.GetPhaseMask(), bot.GetPositionX(),
                bot.GetPositionY(), bot.GetPositionZ(), now);
            if (!place)
                return false;
            ReleaseRepairMovement(owner, state);
            state.repairRoute = place->repair;
            state.nearestRepairDistance = bot.GetExactDist(place->repair->x, place->repair->y, place->repair->z);
            state.lastRepairProgressMs = now;
            state.nextRepairMovementMs = now;
        }
        auto const& route = *state.repairRoute;
        if (route.map != bot.GetMapId() || !(route.phase & bot.GetPhaseMask()))
            return false;
        float const distance = bot.GetExactDist(route.x, route.y, route.z);
        if (distance <= 5.0f || distance > 600.0f)
            return false; // Arrival without a visible repairer is a failed lead, never permission to repair remotely.
        if (distance + 5.0f < state.nearestRepairDistance)
        {
            state.nearestRepairDistance = distance;
            state.lastRepairProgressMs = now;
        }
        else if (now >= state.lastRepairProgressMs && now - state.lastRepairProgressMs >= 90000)
            return false;
        state.availability = "returning_to_known_repairer";
        if (!Publish(owner, state, realMs))
            return true;
        if (now >= state.nextRepairMovementMs)
        {
            state.nextRepairMovementMs = now + 2000;
            float x = 0, y = 0, z = 0;
            bool const continuing = bot.isMoving()
                && bot.GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE
                && bot.GetMotionMaster()->GetDestination(x, y, z) && x == route.x && y == route.y && z == route.z;
            if (!continuing)
                bot.GetMotionMaster()->MovePoint(0, route.x, route.y, route.z, FORCED_MOVEMENT_NONE,
                    0.0f, 0.0f, true, false);
        }
        return true;
    }

    bool PrepareEquipment(ActorKey owner, Owner& state, Player& bot, PlayerbotAI& ai, uint64_t now, uint64_t realMs)
    {
        auto& book = state.book;
        auto const* objective = book.Preparing();
        if (objective && objective->preparation->kind != PreparationKind::RepairEquipment)
            return false;
        if (!objective && CanPrepareResources(bot)
            && CurrentControlMode(bot) == ControlMode::AutonomousSolo)
            for (auto const& [id, candidate] : book.All())
                if (book.CanRepair(id, now) && NeedsEquipmentRepair(bot))
                {
                    CapabilityContext context{owner, state.generation, id, candidate.revision,
                        true, {candidate.quest}, {}, {}};
                    CapabilityRequest request{1, owner, state.generation, id, candidate.revision,
                        "repair_equipment", candidate.quest, 0, {}};
                    if (capabilities.Validate(request, context).empty()
                        && book.BeginRepair(id, candidate.revision, now))
                    {
                        objective = book.Preparing();
                        break;
                    }
                }
        if (!objective)
        {
            ReleaseRepairMovement(owner, state);
            return false;
        }
        auto const id = objective->id;
        auto& control = ai.rpgInfo.objectiveControl;
        if (control.token)
            Release(&bot, control.token);
        control.cooperationHold = true;
        book.ObserveRepair(id, NeedsEquipmentRepair(bot), now);
        if (CurrentControlMode(bot) != ControlMode::AutonomousSolo)
            book.DeferPreparation(id, "Cooperative or human control superseded solo resource preparation", now);
        if (!book.Preparing())
        {
            ReleaseRepairMovement(owner, state);
            control.cooperationHold = false;
            if (auto const* definition = sObjectMgr->GetQuestTemplate(objective->quest))
                book.ResolveReadiness(id, Readiness(bot, *definition, control), now);
            Publish(owner, state, realMs);
            return false;
        }
        state.availability = "preparing_equipment";
        if (!CanPrepareResources(bot))
        {
            Publish(owner, state, realMs);
            return true;
        }
        auto* repairer = VisibleRepairer(bot);
        if (!bot.GetMoney() && FundsWithJunk(bot) == 0)
            book.DeferPreparation(id, "I have no money to pay a repairer; seek affordable work before retrying", now);
        else if (!repairer)
        {
            if (ReturnToKnownRepair(owner, state, bot, now, realMs))
                return true;
            book.DeferPreparation(id,
                "No usable visible or personally known repairer; retain the quest and investigate other work", now);
        }
        else if (!bot.GetNPCIfCanInteractWith(repairer->GetGUID(), UNIT_NPC_FLAG_REPAIR))
        {
            if (!Publish(owner, state, realMs))
                return true;
            if (state.repairRoute || state.repairer != repairer->GetGUID()
                || bot.GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                ReleaseRepairMovement(owner, state);
                state.repairer = repairer->GetGUID();
                bot.GetMotionMaster()->MoveFollow(repairer, INTERACTION_DISTANCE - 1.0f, 0.0f);
            }
        }
        else
        {
            ReleaseRepairMovement(owner, state);
            auto const repairerGuid = repairer->GetGUID();
            if (book.PreparationTransaction(id, now))
            {
                if (!Publish(owner, state, realMs))
                    return true;
                auto const before = bot.GetMoney();
                RepairEquippedItems(bot, repairerGuid);
                book.PreparationExpense(id, before, bot.GetMoney());
                if (NeedsEquipmentRepair(bot))
                {
                    auto const vendor = VisibleMerchant(bot);
                    if (vendor && ReadMerchant(owner, bot, vendor))
                    {
                        SellPreparationJunk(bot, vendor, MAX_MONEY_AMOUNT - 1, book, id);
                        auto const funded = bot.GetMoney();
                        RepairEquippedItems(bot, repairerGuid);
                        book.PreparationExpense(id, funded, bot.GetMoney());
                    }
                }
                book.ObserveRepair(id, NeedsEquipmentRepair(bot), now);
            }
            if (book.Preparing())
                book.DeferPreparation(id, "Equipped items still need repair after the bounded transaction attempt",
                    now);
        }
        if (!book.Preparing())
        {
            ReleaseRepairMovement(owner, state);
            control.cooperationHold = false;
            if (auto const* definition = sObjectMgr->GetQuestTemplate(objective->quest))
                book.ResolveReadiness(id, Readiness(bot, *definition, control), now);
        }
        Publish(owner, state, realMs);
        return book.Preparing() != nullptr;
    }

    void PrepareCandidates(Owner& state, Player& bot, uint64_t now)
    {
        for (auto const quest : QuestLog(bot))
        {
            auto const* definition = sObjectMgr->GetQuestTemplate(quest);
            if (definition && !definition->IsRepeatable())
            {
                auto const* candidate = state.book.ProposeQuest(quest, "Earn the reward for " + definition->GetTitle(),
                    "Accepted quest in my own log", Sample(bot, quest));
                auto* ai = sPlayerbotsMgr.GetPlayerbotAI(&bot);
                if (!candidate || !ai)
                    continue;
                if (!candidate->satisfactionReceipt)
                    state.book.AccountQuestProgress(candidate->id, Sample(bot, quest));
                auto const readiness = Readiness(bot, *definition, ai->rpgInfo.objectiveControl);
                auto const obstruction = QuestReadinessObstruction(readiness);
                if (obstruction == Obstruction::None)
                    state.book.ResolveReadiness(candidate->id, readiness, now);
                else if (state.book.Block(candidate->id, obstruction, ReadinessReason(readiness), now))
                {
                    Release(&bot, candidate->id);
                    state.book.Defer(candidate->id, now);
                }
                state.book.ObserveRepair(candidate->id, readiness.criticallyDamagedEquipment, now);
                if (candidate->preparation && candidate->preparation->kind == PreparationKind::BuyQuestSupplies)
                    state.book.ObserveSupplies(candidate->id,
                        bot.GetItemCount(candidate->preparation->item, false), now);
                if (obstruction == Obstruction::Supplies && !readiness.turningIn
                    && readiness.criticallyDamagedEquipment)
                    state.book.ProposeRepair(candidate->id);
                state.book.ObservePreparationFunds(candidate->id, bot.GetMoney(), now);
            }
        }
        if (brain && now >= state.nextContactSampleMs)
        {
            state.nextContactSampleMs = now + 10000;
            unsigned scanned = 0;
            if (auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap())
                for (auto const& [guid, object] : *visible)
                {
                    if (++scanned > 256)
                        break;
                    if (auto* person = object ? object->ToPlayer() : nullptr;
                        person && person != &bot && person->IsInWorld() && person->IsAlive()
                        && person->IsInMap(&bot) && person->InSamePhase(&bot) && bot.CanSeeOrDetect(person)
                        && bot.IsFriendlyTo(person) && bot.GetExactDist(person) <= 40)
                    {
                        auto const* area = sAreaTableStore.LookupEntry(person->GetAreaId());
                        if (!area || !state.knowledge.LearnPlace(area->ID, PlayerbotAI::GetLocalizedAreaName(area)))
                            continue;
                        state.knowledge.RememberContact({{ActorKey{ActorKind::Player, person->GetGUID().GetRawValue()},
                            person->GetName()}, area->ID, {person->GetMapId(), person->GetPhaseMask(),
                            person->GetPositionX(), person->GetPositionY(), person->GetPositionZ(), now}});
                    }
                }
        }
        if (brain)
        {
            for (auto const& [id, objective] : state.book.All())
                state.book.ReconsiderActivity(id, now);
            if (state.currentArea && now >= state.satisfaction.Capture().nextRestMs)
                state.book.ProposeActivity(state.currentArea, PlacePurpose::Rest,
                    "Rest here", "Consider recovering through observed stationary rest");
            std::vector<KnownContact const*> contacts;
            for (auto const& [actor, contact] : state.knowledge.Contacts())
                if (now >= state.satisfaction.Capture().nextSocialMs && now >= contact.location.observedMs
                    && now - contact.location.observedMs <= 600000 && contact.location.map == bot.GetMapId()
                    && (contact.location.phase & bot.GetPhaseMask()))
                    contacts.push_back(&contact);
            std::stable_sort(contacts.begin(), contacts.end(), [&](auto const* left, auto const* right)
            {
                auto distance = [&](KnownContact const* contact)
                {
                    auto const& point = contact->location;
                    return bot.GetExactDist(point.x, point.y, point.z);
                };
                return distance(left) < distance(right);
            });
            for (std::size_t index = 0; index < std::min(contacts.size(), std::size_t(2)); ++index)
            {
                auto const& contact = *contacts[index];
                state.book.ProposeActivity(contact.place, PlacePurpose::Companionship,
                    "Visit " + contact.person.name, "Look for a companion at the personally observed meeting site",
                    contact.person.actor);
            }
            std::vector<std::pair<double, uint32_t>> discoveries;
            for (auto const& [area, place] : state.knowledge.Places())
                if (!place.visitedMs)
                    if (auto const point = ResolveArea(bot, area); point != WorldPosition()
                        && point.GetMapId() == bot.GetMapId())
                        discoveries.emplace_back(bot.GetExactDist(point), area);
            std::sort(discoveries.begin(), discoveries.end());
            for (std::size_t index = 0; index < std::min(discoveries.size(), std::size_t(2)); ++index)
            {
                auto const& place = state.knowledge.Places().at(discoveries[index].second);
                state.book.ProposeActivity(place.area, PlacePurpose::Discovery,
                    "Discover " + place.name, "Observe a privately known place I have not visited");
            }
        }
        auto choices = state.knowledge.Alternatives(state.currentArea, uint8_t(bot.GetLevel()));
        if (auto const local = state.knowledge.Places().find(state.currentArea);
            local != state.knowledge.Places().end())
            choices.insert(choices.begin(), &local->second);
        unsigned count = 0;
        for (auto const* place : choices)
        {
            if (++count > 6)
                break;
            // Preserve an exhausted/completed area's existing intentions; new knowledge or retries reopen work.
            bool const known = std::any_of(state.book.All().begin(), state.book.All().end(), [&](auto const& item)
                { return item.second.place == place->area && !item.second.quest
                    && item.second.purpose == PlacePurpose::Work; });
            if (!known)
                state.book.ProposePlace(place->area, "Discover work in " + place->name,
                    place->area == state.currentArea ? "Investigate local opportunities before leaving"
                        : "Consider a suitable place from my private geography");
        }
    }

    struct ActivityRoute
    {
        uint64_t durationMs = 0;
        double risk = 0;
        std::vector<WorldPosition> stops;
    };

    struct PerceivedThreat
    {
        G3D::Vector3 position;
        double risk;
    };

    std::vector<PerceivedThreat> PerceivedThreats(Player& bot) const
    {
        std::vector<PerceivedThreat> result;
        unsigned scanned = 0;
        if (auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap())
            for (auto const& [guid, object] : *visible)
            {
                if (++scanned > 256 || result.size() >= 16)
                    break;
                auto const* creature = object ? object->ToCreature() : nullptr;
                if (creature && creature->IsAlive() && bot.IsHostileTo(creature)
                    && bot.CanSeeOrDetect(creature) && creature->InSamePhase(&bot) && bot.GetExactDist(creature) <= 80)
                    result.push_back({{creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()},
                        creature->GetLevel() > bot.GetLevel() + 2 ? 0.3 : 0.08});
            }
        return result;
    }

    std::optional<ActivityRoute> RouteEstimate(Player& bot, std::vector<WorldPosition> const& stops,
        std::vector<PerceivedThreat> const& threats) const
    {
        ActivityRoute result;
        result.stops = stops;
        auto from = WorldPosition(&bot);
        double length = 0;
        std::set<std::size_t> encountered;
        for (auto const& destination : stops)
        {
            if (destination == WorldPosition() || destination.GetMapId() != bot.GetMapId())
                return std::nullopt;
            PathGenerator path(&bot);
            path.SetUseStraightPath(true);
            if (!path.CalculatePath(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(),
                destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), false)
                || (path.GetPathType() & ~uint32_t(PATHFIND_NORMAL | PATHFIND_INCOMPLETE)))
                return std::nullopt;
            auto const& points = path.GetPath();
            double legLength = 0;
            for (std::size_t index = 1; index < points.size(); ++index)
            {
                auto const segment = points[index] - points[index - 1];
                legLength += segment.length();
                for (std::size_t threat = 0; threat < threats.size(); ++threat)
                {
                    // Evaluate the entire corridor segment, including long stretches between navmesh corners.
                    auto const offset = threats[threat].position - points[index - 1];
                    float const t = segment.squaredLength() > 0
                        ? std::clamp(offset.dot(segment) / segment.squaredLength(), 0.0f, 1.0f) : 0;
                    if ((offset - segment * t).length() < 25)
                        encountered.insert(threat);
                }
            }
            length += std::max(legLength, double(from.distance(destination)));
            if (path.GetPathType() & PATHFIND_INCOMPLETE)
                result.risk = std::max(result.risk, 0.25);
            from = destination;
        }
        // Staying near a personally visible threat also carries risk.
        if (length < 5)
            for (std::size_t index = 0; index < threats.size(); ++index)
                if (bot.GetExactDist(threats[index].position.x, threats[index].position.y,
                    threats[index].position.z) < 25)
                    encountered.insert(index);
        for (auto const index : encountered)
            result.risk = std::min(0.8, result.risk + threats[index].risk);
        auto const speed = std::max(1.0f, bot.GetSpeed(MOVE_RUN));
        result.durationMs = uint64_t(std::min(3600000.0, 1000 * length / speed));
        return result;
    }

    void AssessActivities(Owner& state, Player& bot, PlayerbotAI& ai, uint64_t now)
    {
        if (!brain)
            return;
        auto const* current = state.book.Current();
        if (now < state.nextAssessmentMs && state.intention == (current ? current->id : 0))
            return;
        state.nextAssessmentMs = now + 5000;
        if (state.intention != (current ? current->id : 0))
        {
            state.intention = current ? current->id : 0;
            state.intentionSinceMs = now;
            state.activityObservedMs = state.activityArrivalObservedMs = 0;
        }
        state.destinations.clear();
        state.travelTimes.clear();
        state.routes.clear();
        state.routeRisks.clear();
        state.routeReasons.clear();
        auto const threats = PerceivedThreats(bot);
        std::vector<SatisfactionCandidate> candidates;
        auto const circumstances = Circumstances(bot);
        auto const finances = OwnQuestFinances(bot);
        for (auto const& [id, objective] : state.book.All())
        {
            if (objective.request || (state.partyQuest && objective.quest != state.partyQuest)
                || (objective.state != ObjectiveState::Proposed && objective.state != ObjectiveState::Active
                    && objective.state != ObjectiveState::Waiting
                    && !state.book.Retryable(objective, now, circumstances)))
                continue;
            bool const supplies = state.book.CanBuySupplies(id, now) && state.merchant
                && now >= state.merchantReceivedMs && now - state.merchantReceivedMs < 30000
                && SupplyPurchase(bot, objective, *state.merchant, true);
            if (objective.quest && !supplies && !ReadyToAttempt(bot, objective.quest, ai.rpgInfo.objectiveControl))
                continue;
            uint64_t travelMs = 0;
            double risk = 0;
            WorldPosition destination;
            if (objective.quest)
            {
                if (auto const* task = std::get_if<NewRpgInfo::DoQuest>(&ai.rpgInfo.data);
                    task && task->questId == objective.quest && task->pos != WorldPosition())
                    destination = task->pos;
                else
                    travelMs = 120000; // Unknown route: a bounded prior, never a free journey.
            }
            else if (objective.purpose == PlacePurpose::Rest)
            {
                if (objective.place != state.currentArea || now < state.satisfaction.Capture().nextRestMs)
                    continue;
                destination = WorldPosition(&bot);
            }
            else if (objective.purpose == PlacePurpose::Companionship)
            {
                auto const contact = objective.person ? state.knowledge.Contacts().find(*objective.person)
                    : state.knowledge.Contacts().end();
                if (contact == state.knowledge.Contacts().end() || now < contact->second.location.observedMs
                    || now - contact->second.location.observedMs > 600000
                    || now < state.satisfaction.Capture().nextSocialMs
                    || !(contact->second.location.phase & bot.GetPhaseMask()))
                    continue;
                auto const& location = contact->second.location;
                destination = WorldPosition(location.map, location.x, location.y, location.z, 0);
            }
            else
            {
                auto const known = state.knowledge.Places().find(objective.place);
                if (known == state.knowledge.Places().end()
                    || (objective.purpose == PlacePurpose::Discovery && known->second.visitedMs
                        && state.discoveredArea != objective.place))
                    continue;
                destination = objective.place == state.currentArea ? WorldPosition(&bot)
                    : ResolveArea(bot, objective.place);
                if (destination == WorldPosition())
                    continue;
            }
            if (destination != WorldPosition())
            {
                if (destination.GetMapId() != bot.GetMapId())
                    continue;
                travelMs = uint64_t(std::min(3600000.0,
                    double(bot.GetExactDist(destination)) * 1000 / std::max(1.0f, bot.GetSpeed(MOVE_RUN))));
                state.destinations[id] = destination;
            }
            state.travelTimes[id] = travelMs;
            auto const activity = objective.quest ? "pursue_quest" : ActivityCapability(objective.purpose);
            auto effects = state.satisfaction.Effects(activity);
            if (objective.quest && objective.cooperation.state == CooperationState::Working)
                for (auto const& [dimension, effect] : state.satisfaction.Effects("help_companion"))
                    effects[dimension] = std::clamp(effects[dimension] + effect, -1.0, 1.0);
            if (objective.purpose == PlacePurpose::Rest)
                effects = state.satisfaction.Effects(activity, double(60000 - objective.activityMs) / 60000);
            if (objective.quest)
                if (auto const money = finances.money.find(objective.quest); money != finances.money.end())
                {
                    auto value = [&](double amount) { return amount / (amount + 1000 * bot.GetLevel()); };
                    effects["resources"] = value(std::max(0.0, double(finances.ownMoney) + money->second))
                        - value(finances.ownMoney);
                }
            double const prior = objective.purpose == PlacePurpose::Rest ? 1
                : objective.checkpoint.readyToReward ? 0.95
                : objective.purpose == PlacePurpose::Discovery ? 0.85
                : objective.purpose == PlacePurpose::Companionship ? 0.65 : 0.6;
            double const success = state.satisfaction.SuccessProbability(activity, prior);
            uint64_t const duration = objective.purpose == PlacePurpose::Rest ? 60000 - objective.activityMs
                : state.satisfaction.ExpectedDuration(activity, objective.checkpoint.readyToReward ? 10000 : 120000)
                    + (supplies ? 30000 : 0);
            candidates.push_back({id, objective.revision,
                ForecastActivity(travelMs, risk, success, duration, effects)});
        }
        // First score all bounded known candidates cheaply. Retain the current intention plus the best of
        // each purpose before filling the eight pathfinding slots; quests cannot consume every route query.
        auto const coarse = state.satisfaction.Choose(candidates);
        std::set<uint64_t> shortlist;
        if (current)
            shortlist.insert(current->id);
        std::set<std::pair<bool, PlacePurpose>> purposes;
        for (auto const& assessed : coarse.alternatives)
        {
            auto const* objective = state.book.Find(assessed.id);
            if (purposes.insert({bool(objective->quest), objective->purpose}).second)
                shortlist.insert(assessed.id);
        }
        for (auto const& assessed : coarse.alternatives)
            if (shortlist.size() < 8)
                shortlist.insert(assessed.id);
        std::vector<SatisfactionCandidate> routed;
        for (auto const& candidate : candidates)
        {
            if (!shortlist.contains(candidate.id))
                continue;
            auto const destination = state.destinations.find(candidate.id);
            if (destination == state.destinations.end())
            {
                // Accepted work without a known executor destination retains its explicit uncertain prior.
                state.routeReasons[candidate.id] = "unknown route; duration prior";
                routed.push_back(candidate);
                continue;
            }
            auto const direct = RouteEstimate(bot, {destination->second}, threats);
            if (!direct)
            {
                state.routeReasons[candidate.id] = "no navigable corridor; destination remains privately known";
                continue;
            }
            auto const& effects = candidate.forecast.outcomes.front().stages.back().effects;
            auto const duration = candidate.forecast.outcomes.front().stages.back().durationMs;
            auto const probability = candidate.forecast.outcomes.front().probability;
            auto bestRoute = *direct;
            auto bestForecast = ForecastActivity(direct->durationMs, direct->risk, probability, duration, effects);
            auto const assessedRoute = state.satisfaction.Evaluate(bestForecast);
            if (!assessedRoute)
                continue;
            auto bestValue = assessedRoute->total;
            auto const* objective = state.book.Find(candidate.id);
            // Bounded alternatives use geometry around a threat the owner can actually perceive. The waypoint
            // is route policy for this intention, never an invented discovery or a second activity reward.
            if (!objective->quest && objective->purpose != PlacePurpose::Rest && direct->risk > 0 && !threats.empty())
            {
                auto const& threat = threats.front().position;
                double const angle = std::atan2(destination->second.GetPositionY() - bot.GetPositionY(),
                    destination->second.GetPositionX() - bot.GetPositionX()) + M_PI / 2;
                for (int side : {-1, 1})
                {
                    float const x = threat.x + side * 40 * std::cos(angle);
                    float const y = threat.y + side * 40 * std::sin(angle);
                    float z = threat.z;
                    bot.UpdateAllowedPositionZ(x, y, z);
                    auto const detour = RouteEstimate(bot,
                        {WorldPosition(bot.GetMapId(), x, y, z, 0), destination->second}, threats);
                    if (!detour)
                        continue;
                    auto forecast = ForecastActivity(detour->durationMs, detour->risk, probability, duration, effects);
                    auto const assessedDetour = state.satisfaction.Evaluate(forecast);
                    if (!assessedDetour)
                        continue;
                    auto const value = assessedDetour->total;
                    if (value > bestValue + 0.01)
                    {
                        bestValue = value;
                        bestRoute = *detour;
                        bestForecast = std::move(forecast);
                    }
                }
            }
            state.routes[candidate.id] = bestRoute.stops;
            state.routeRisks[candidate.id] = bestRoute.risk;
            state.travelTimes[candidate.id] = bestRoute.durationMs;
            state.routeReasons[candidate.id] = bestRoute.stops.size() > 1
                ? "safer corridor around a perceived threat" : "direct navigable corridor";
            routed.push_back({candidate.id, candidate.revision, std::move(bestForecast)});
        }
        bool const danger = current && (bot.GetHealthPct() < 35
            || (state.routeRisks.contains(current->id) && state.routeRisks.at(current->id) >= 0.3));
        bool const committed = current && !danger && now >= state.intentionSinceMs
            && now - state.intentionSinceMs < 120000;
        state.satisfactionDecision = state.satisfaction.Choose(routed, current ? current->id : 0, committed);
        // Preserve the committed corridor across ordinary reevaluation. A newly perceived danger can install
        // a materially safer route, while the body keeps ownership of the same activity.
        if (danger && current && state.satisfactionDecision.selected == current->id
            && state.routes.contains(current->id) && state.activeRoute.size() == 1
            && state.routes.at(current->id).size() > 1)
        {
            Release(&bot, current->id);
            state.activeRoute = state.routes.at(current->id);
            state.routeIndex = 0;
        }
    }

    void ExecuteActivity(Owner& state, Player& bot, PlayerbotAI& ai, uint64_t now)
    {
        auto const* objective = state.book.Current();
        if (!objective || objective->purpose == PlacePurpose::Work || objective->state != ObjectiveState::Active)
            return;
        auto const id = objective->id;
        ActivityObservation observation;
        observation.area = state.currentArea;
        observation.available = Autonomous(bot, ai) && bot.IsAlive() && !bot.IsInCombat()
            && !bot.IsBeingTeleported() && !bot.IsInFlight() && !bot.IsNonMeleeSpellCast(false)
            && !bot.IsSitState() && !bot.HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_ROOT);
        if (!observation.available)
        {
            state.book.ObserveActivity(id, observation, now);
            return;
        }
        observation.discovered = state.discoveredArea == objective->place;
        if (objective->purpose == PlacePurpose::Discovery && observation.discovered)
        {
            state.book.ObserveActivity(id, observation, now);
            Release(&bot, id);
            return;
        }
        if (state.activeRoute.empty())
        {
            auto const route = state.routes.find(id);
            if (route == state.routes.end() || route->second.empty())
            {
                state.book.Block(id, Obstruction::Navigation, "No feasible route to this activity's destination", now);
                return;
            }
            state.activeRoute = route->second;
            state.routeIndex = 0;
        }
        bool companionNearby = false;
        if (objective->purpose == PlacePurpose::Companionship && objective->person)
            if (auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap())
                if (auto const found = visible->find(ObjectGuid(HighGuid::Player, uint32_t(objective->person->id)));
                    found != visible->end() && found->second)
                    companionNearby = bot.CanSeeOrDetect(found->second) && bot.GetExactDist(found->second) <= 20;
        if (companionNearby && state.routeIndex < state.activeRoute.size())
        {
            Release(&bot, id);
            ai.rpgInfo.objectiveControl.ClaimPlace(id, objective->place);
            state.routeIndex = state.activeRoute.size();
        }
        while (state.routeIndex < state.activeRoute.size()
            && bot.GetExactDist(state.activeRoute[state.routeIndex]) < 5)
            ++state.routeIndex;
        if (state.routeIndex < state.activeRoute.size())
        {
            ai.rpgInfo.objectiveControl.phase = QuestObjectiveControl::Phase::Traveling;
            if (ai.rpgInfo.body.Fresh(getMSTime()) && ai.rpgInfo.body.objective == id)
                CooperativeMovement(&ai).Walk(state.activeRoute[state.routeIndex]);
            return;
        }
        ai.rpgInfo.objectiveControl.phase = QuestObjectiveControl::Phase::Attempting;
        observation.resting = objective->purpose == PlacePurpose::Rest && !bot.isMoving()
            && !bot.IsNonMeleeSpellCast(false);
        if (objective->purpose == PlacePurpose::Companionship && objective->person
            && now >= state.nextActivityInteractionMs)
        {
            auto const* visible = bot.GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
            auto const guid = ObjectGuid(HighGuid::Player, uint32_t(objective->person->id));
            Player* companion = nullptr;
            if (visible)
                if (auto const found = visible->find(guid); found != visible->end() && found->second)
                    companion = found->second->ToPlayer();
            if (companion && bot.CanSeeOrDetect(companion) && companion->IsAlive()
                && companion->InSamePhase(&bot) && companion->IsInMap(&bot) && bot.IsFriendlyTo(companion)
                && bot.GetExactDist(companion) <= 20)
            {
                state.nextActivityInteractionMs = now + 30000;
                observation.person = objective->person;
                observation.interaction = SendNormalChat(bot, *companion, SpeechRoute{CHAT_MSG_SAY},
                    "Hello, " + companion->GetName() + ". It is good to see you.");
            }
        }
        state.book.ObserveActivity(id, observation, now);
        if (objective->state == ObjectiveState::Completed)
            Release(&bot, id);
        else if (objective->purpose != PlacePurpose::Rest && state.activityArrivalObservedMs >= 60000)
            state.book.Block(id, Obstruction::Information,
                "The expected observation or companion was not found at the destination", now);
    }

    void ObserveSatisfaction(ActorKey owner, Owner& state, uint64_t now, uint64_t realMs)
    {
        if (!brain || !state.generation)
            return;
        auto* bot = Find(owner);
        auto* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        if (!ai || !Autonomous(*bot, *ai) || now <= state.satisfaction.Capture().observedMs)
        {
            state.satisfactionSampleMs = 0;
            return;
        }
        uint64_t const elapsed = state.satisfactionSampleMs && now >= state.satisfactionSampleMs
            && now - state.satisfactionSampleMs <= 2000 ? now - state.satisfactionSampleMs : 0;
        state.satisfactionSampleMs = now;
        if (elapsed && !bot->IsBeingTeleported() && !bot->IsInFlight())
        {
            // Learning estimates execution time separately from the remaining route cost in forecasts.
            if (auto const* intention = state.book.Find(state.intention);
                intention && (intention->step == ObjectiveStep::Attempt || bot->IsInCombat()))
                state.activityObservedMs = std::min(uint64_t(3600000), state.activityObservedMs + elapsed);
            if (auto const* current = state.book.Current(); current && current->arrivedMs && !bot->isMoving()
                && bot->IsAlive() && !bot->IsInCombat())
                state.activityArrivalObservedMs = std::min(uint64_t(60000), state.activityArrivalObservedMs + elapsed);
        }
        SatisfactionEffects effects;
        auto add = [&](SatisfactionEffects const& additions)
        {
            for (auto const& [dimension, effect] : additions)
                effects[dimension] = std::clamp(effects[dimension] + effect, -1.0, 1.0);
        };
        if (elapsed && bot->isMoving() && state.satisfaction.Capture().dimensions.contains("rest"))
            effects["rest"] = -double(elapsed) / 3600000 * 0.2;
        if (elapsed && state.discoveredArea)
            add(state.satisfaction.Effects("explore_place"));
        for (auto const& [id, objective] : state.book.All())
        {
            if (objective.quest)
            {
                auto const [credit, reward] = state.book.AccountQuestProgress(id, Sample(*bot, objective.quest));
                if (credit || reward)
                {
                    add(state.satisfaction.Effects("pursue_quest", reward ? 1.0 : std::min(0.5, credit * 0.05)));
                    if (objective.cooperation.state == CooperationState::Working
                        || objective.cooperation.state == CooperationState::Completed)
                        add(state.satisfaction.Effects("help_companion", reward ? 1.0 : std::min(0.5, credit * 0.05)));
                }
            }
            if (objective.purpose == PlacePurpose::Rest)
                if (auto const rested = state.book.AccountRest(id))
                    add(state.satisfaction.Effects("rest", double(rested) / 60000));
            bool const completed = objective.state == ObjectiveState::Completed;
            bool const failed = objective.state == ObjectiveState::Deferred
                && objective.obstruction != Obstruction::None;
            if ((completed || failed) && state.book.AssessAttempt(id))
            {
                auto const activity = objective.quest ? "pursue_quest" : ActivityCapability(objective.purpose);
                if (state.intention == id && state.activityObservedMs)
                    state.satisfaction.Learn(activity, completed, state.activityObservedMs);
                if (completed && objective.purpose == PlacePurpose::Companionship
                    && state.satisfaction.ActivityReceipt(activity, now))
                    add(state.satisfaction.Effects(activity));
                if (completed && !objective.quest && objective.purpose == PlacePurpose::Work && !objective.request)
                    add(state.satisfaction.Effects("discover_work"));
                if (completed && objective.purpose == PlacePurpose::Rest)
                    state.satisfaction.ActivityReceipt(activity, now);
            }
        }
        auto const& dimensions = state.satisfaction.Capture().dimensions;
        if (dimensions.contains("security"))
            effects["security"] = double(bot->GetHealthPct()) / 100 - dimensions.at("security").fulfillment;
        if (dimensions.contains("resources"))
            effects["resources"] = double(bot->GetMoney()) / (double(bot->GetMoney()) + 1000 * bot->GetLevel())
                - dimensions.at("resources").fulfillment;
        state.satisfaction.Observe(now, elapsed, effects);
        Publish(owner, state, realMs);
    }

    bool CanSeekInformation(ActorKey owner, Owner const& state, Player const& bot) const
    {
        auto const* current = state.book.Current();
        return conversation && !bot.IsInCombat() && (!current || !current->quest) && conversation->CanAsk(owner);
    }

    void ProcessDecision(ActorKey owner, Owner& state, Player& bot, uint64_t now, uint64_t realMs)
    {
        if (!bridge || !state.decision)
            return;
        auto& pending = *state.decision;
        auto const* anchor = state.book.Find(pending.job.issued.objective);
        bool const stale = !anchor || anchor->revision != pending.job.issued.revision
            || state.generation != pending.job.issued.actorGeneration || realMs >= pending.expiresRealMs;
        if (!stale && !pending.result)
            return;
        ObjectiveChoice choice{"worker_expired_or_stale"};
        if (!stale && pending.result->status == "success" && !bot.IsInCombat() && bot.IsAlive()
            && !bot.IsBeingTeleported() && !bot.IsInFlight())
        {
            auto live = pending.job.issued;
            live.revision = anchor->revision;
            live.actorGeneration = state.generation;
            try
            {
                auto decision = Bridge::DecodePlanningDecision(pending.result->response, pending.job.issued);
                choice = ApplyObjectiveChoice(pending.job, decision, live, state.book, state.knowledge,
                    Circumstances(bot), CanSeekInformation(owner, state, bot), now,
                    brain ? &state.satisfactionDecision : nullptr);
            }
            catch (std::exception const&)
            {
                choice.status = "invalid_planning_result";
            }
        }
        else if (!stale && pending.result->status != "success")
            choice.status = "worker_" + pending.result->status;
        if (recorder)
            recorder->Record(owner, "alles_plan", pending.job.issued.objective, choice.status,
                boost::json::serialize(boost::json::object{{"job", pending.id}, {"selected", choice.selected},
                    {"released", choice.release}, {"question", choice.question}}), realMs);
        bridge->CancelPlanning(pending.id);
        state.decision.reset();
        if (stale)
            state.lastPlannedSignal = 0; // A fresh observation may retry after the per-owner cooldown.
        if (choice.release)
            Release(&bot, choice.release);
        if (choice.question)
            AskForInformation(owner, state, bot, now, realMs, choice.question);
    }

    void QueueDecision(ActorKey owner, Owner& state, Player& bot, uint64_t now, uint64_t realMs)
    {
        if (!bridge || state.advice || state.decision || !state.replies.empty()
            || bot.IsInCombat() || !bot.IsAlive() || bot.IsBeingTeleported() || bot.IsInFlight())
            return;
        auto const circumstances = Circumstances(bot);
        auto const finances = OwnQuestFinances(bot);
        auto signal = ObjectiveDecisionSignal(state.book, state.knowledge,
            uint8_t(bot.GetLevel()), state.currentArea, circumstances, now, finances);
        if (brain)
            signal ^= state.satisfactionDecision.selected * 1099511628211ULL;
        if (signal != state.seenDecisionSignal)
        {
            state.seenDecisionSignal = signal;
            state.decisionSignalSinceMs = realMs;
        }
        if (signal == state.lastPlannedSignal || realMs < state.nextPlanningRealMs
            || realMs - state.decisionSignalSinceMs < 2000)
            return;
        state.nextPlanningRealMs = realMs + 30000;
        auto job = PrepareObjectivePlanning(owner, state.generation, state.book, state.knowledge,
            uint8_t(bot.GetLevel()), state.currentArea, circumstances,
            CanSeekInformation(owner, state, bot), now, finances, brain ? &state.satisfactionDecision : nullptr);
        if (!job)
            return;
        auto id = "decision-" + std::to_string(owner.id) + "-" + std::to_string(++nextAdviceId);
        if (bridge->QueuePlanning(id, job->context, realMs))
        {
            state.lastPlannedSignal = signal;
            state.decision = PendingDecision{std::move(id), std::move(*job), realMs + 45000, {}};
        }
    }

    void Detach(ActorKey owner, uint64_t now, uint64_t realMs)
    {
        auto found = states.find(owner);
        if (found == states.end())
            return;
        if (bridge && found->second.advice)
            bridge->CancelPlanning(found->second.advice->id);
        if (bridge && found->second.decision)
            bridge->CancelPlanning(found->second.decision->id);
        found->second.decision.reset();
        found->second.lastPlannedSignal = 0;
        found->second.advice.reset();
        found->second.replies.clear();
        if (conversation)
            conversation->TakeInformationReplies(owner);
        if (auto* bot = Find(owner))
        {
            if (CurrentControlMode(*bot) == ControlMode::Human)
            {
                if (auto const* preparing = found->second.book.Preparing())
                    found->second.book.DeferPreparation(preparing->id,
                        "Human control superseded resource preparation", now);
                if (auto const* following = found->second.book.Following())
                    found->second.book.Cancel(following->id, "Human control superseded the conversation request");
                for (auto const& [id, objective] : found->second.book.All())
                {
                    auto cooperation = objective.cooperation;
                    PartyObservation handoff;
                    handoff.ownerControlled = true;
                    if (ObserveCooperation(cooperation, handoff, now))
                        found->second.book.SetCooperation(id, objective.revision, std::move(cooperation));
                }
            }
            if (auto* ai = sPlayerbotsMgr.GetPlayerbotAI(bot))
            {
                if (ai->rpgInfo.body.Detach(found->second.generation, found->second.attachment))
                    ai->rpgInfo.bodyTravel = {};
                ai->rpgInfo.objectiveControl.plannerAttached = false;
                ai->rpgInfo.objectiveControl.cooperativeQuest = 0;
                ai->rpgInfo.objectiveControl.partyMembers = 0;
                ai->rpgInfo.objectiveControl.cooperationHold = false;
                ai->rpgInfo.objectiveControl.cooperativeTurnIn = false;
            }
        }
        ReleaseFollow(owner, found->second);
        ReleaseRepairMovement(owner, found->second);
        if (auto const* current = found->second.book.Current())
        {
            Release(Find(owner), current->id);
            found->second.book.Suspend(current->id, ObjectiveStep::Wait,
                "Execution detached; reconcile on return", now);
        }
        found->second.routeTarget = WorldPosition();
        found->second.satisfactionSampleMs = 0;
        found->second.activeRoute.clear();
        found->second.routeIndex = 0;
        found->second.navigationOrigin = WorldPosition();
        found->second.failedRoutes.clear();
        found->second.merchant.reset();
        found->second.lastMerchant.Clear();
        found->second.merchantReceivedMs = 0;
        found->second.partyDestination = WorldPosition();
        found->second.partyInstance = 0;
        found->second.partyInstanceObjective = 0;
        found->second.notifiedRoster.clear();
        found->second.rosterRecipients.clear();
        found->second.survey = {};
        Publish(owner, found->second, realMs);
    }

    void Tick(ActorKey owner, Owner& state, uint64_t now, uint64_t realMs)
    {
        state.discoveredArea = 0;
        auto* bot = Find(owner);
        auto* ai = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
        auto const status = store.Status(owner);
        if (!bot || !ai || !status || status->state != ActorState::Ready || !status->attachment)
        {
            Detach(owner, now, realMs);
            state.availability = "waiting_for_owner";
            return;
        }
        auto const* persisted = store.FindReady(owner);
        uint64_t const persistedRevision = persisted && persisted->planning ? persisted->planning->revision : 0;
        if (state.generation != status->generation || state.attachment != status->attachment
            || state.planningRevision != persistedRevision)
        {
            Detach(owner, now, realMs);
            auto const* snapshot = store.FindReady(owner);
            ObjectiveBook book;
            PrivateKnowledge knowledge;
            if (!snapshot || (snapshot->planning && (!book.Restore(snapshot->planning->objectives)
                || !knowledge.Restore(snapshot->planning->knowledge))))
            {
                state.availability = "invalid_saved_planning";
                return;
            }
            state.book = std::move(book);
            state.knowledge = std::move(knowledge);
            state.satisfaction.Restore(snapshot->planning ? snapshot->planning->satisfaction : DefaultSatisfaction());
            state.satisfactionSampleMs = state.intention = state.intentionSinceMs = state.activityObservedMs = 0;
            state.destinations.clear();
            state.travelTimes.clear();
            for (auto const& [id, objective] : state.book.All())
            {
                if (objective.request)
                    continue;
                auto cooperation = objective.cooperation;
                ReconcileCooperation(cooperation, now);
                state.book.SetCooperation(id, objective.revision, std::move(cooperation));
                if (objective.quest)
                    state.book.Reconcile(id, Sample(*bot, objective.quest), now);
                else
                    state.book.ReconcilePlace(id, objective.discoveredQuest
                        && (bot->FindQuestSlot(objective.discoveredQuest) < MAX_QUEST_LOG_SIZE
                            || bot->GetQuestRewardStatus(objective.discoveredQuest)), now);
            }
            state.planningRevision = snapshot->planning ? snapshot->planning->revision : 0;
            state.generation = status->generation;
            state.attachment = status->attachment;
            state.currentArea = 0;
            state.observedQuests = QuestLog(*bot);
            state.emitted.clear();
            state.sharedQuest.reset();
        }
        auto& book = state.book;
        book.ExpireQuestions(now);
        auto& control = ai->rpgInfo.objectiveControl;
        control.cooperativeQuest = 0;
        control.partyMembers = 0;
        control.cooperationHold = false;
        control.cooperativeTurnIn = false;
        state.partyQuest = 0;
        if (!Autonomous(*bot, *ai) || !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)
            || ai->HasStrategy("rpg", BOT_STATE_NON_COMBAT) || ai->HasStrategy("travel", BOT_STATE_NON_COMBAT))
        {
            Detach(owner, now, realMs);
            state.availability = "human_control_or_incompatible_engine";
            return;
        }
        if (!book.Preparing())
            ReleaseRepairMovement(owner, state);
        if (FollowRequest(owner, state, *bot, *ai, now, realMs))
            return;
        if (!autonomousPlanning)
        {
            control.plannerAttached = false;
            state.availability = "human_requests_only";
            Publish(owner, state, realMs);
            return;
        }
        // These are the faction arrival turn-ins after the phased death-knight start, verified in local quest data.
        bool const startingComplete = bot->getClass() != CLASS_DEATH_KNIGHT
            || (bot->GetMapId() != 609 && (bot->GetQuestRewardStatus(13188) || bot->GetQuestRewardStatus(13189)));
        if (!startingComplete)
        {
            Detach(owner, now, realMs);
            state.availability = "death_knight_starting_experience";
            return;
        }
        control.plannerAttached = true;
        // Observe actual relocation, not equipment, as evidence that a blocked route may now work.
        // The initial sample after attachment only establishes a baseline; relog is not recovery.
        WorldPosition const position(bot);
        if (state.navigationOrigin == WorldPosition())
            state.navigationOrigin = position;
        else if (state.navigationOrigin.GetMapId() != position.GetMapId()
            || state.navigationOrigin.GetExactDist(position) >= 20.0f)
        {
            book.ReconsiderNavigation(now);
            state.navigationOrigin = position;
        }
        if (auto const* preparing = book.Preparing(); preparing
            && CurrentControlMode(*bot) != ControlMode::AutonomousSolo)
        {
            book.DeferPreparation(preparing->id, "Actual party control superseded solo resource preparation", now);
            ReleaseRepairMovement(owner, state);
        }
        bool const partyOperations = now >= state.nextPartyOperationMs;
        if (partyOperations)
            state.nextPartyOperationMs = now + 5000;
        bool const hasActiveCooperation = std::any_of(book.All().begin(), book.All().end(), [](auto const& item)
        {
            auto const phase = item.second.cooperation.state;
            return phase > CooperationState::None && phase < CooperationState::Completed;
        });
        for (auto const& [id, objective] : book.All())
        {
            if (objective.cooperation.state == CooperationState::None)
                continue;
            auto const ownProgress = Sample(*bot, objective.quest);
            if (!ownProgress.inLog || ownProgress.failed)
                book.Observe(id, ownProgress, ObjectiveStep::Wait, now);
            auto cooperation = objective.cooperation;
            if (partyOperations)
            {
                if (cooperation.state == CooperationState::Agreed || cooperation.state == CooperationState::Rendezvous)
                {
                    AcceptCompanionInvitation(*bot, cooperation, now);
                    if (cooperation.leader == owner)
                    {
                        RecruitParty(owner, state, *bot, cooperation, now, realMs);
                        for (auto const receiver : ShareCooperativeQuest(*bot, cooperation, now))
                            if (auto found = states.find(receiver); found != states.end())
                                found->second.sharedQuest = Owner::SharedQuest{owner, cooperation.quest, now + 120000};
                    }
                    else if (state.sharedQuest && now < state.sharedQuest->expiresMs)
                    {
                        auto const& offer = *state.sharedQuest;
                        if (AcceptSharedCooperativeQuest(*bot, cooperation, offer.source, offer.quest, now))
                            state.sharedQuest.reset();
                    }
                }
                else if (!hasActiveCooperation && (cooperation.state == CooperationState::Deferred
                    || cooperation.state == CooperationState::Cancelled
                    || cooperation.state == CooperationState::Completed))
                {
                    // An old agreement cannot disband a new party that happens to have the same leader/roster.
                    auto const present = ObserveParty(*bot, cooperation);
                    if (state.partyInstanceObjective == id && state.partyInstance
                        && state.partyInstance == present.group)
                        LeaveCooperativeParty(*bot, cooperation, now);
                }
            }
            auto const observation = ObserveParty(*bot, cooperation);
            ObserveCooperation(cooperation, observation, now);
            if (observation.group && objective.cooperation.state < CooperationState::Completed
                && cooperation.state <= CooperationState::Completed)
            {
                state.partyInstance = observation.group;
                state.partyInstanceObjective = id;
            }
            Coordinate(owner, state, *bot, *ai, cooperation, observation, now, realMs);
            if (cooperation != objective.cooperation)
                book.SetCooperation(id, objective.revision, cooperation);
            if (bot->GetQuestRewardStatus(objective.quest))
            {
                book.Observe(id, Sample(*bot, objective.quest), ObjectiveStep::Wait, now);
                Release(bot, id);
            }
        }
        // Rebuild semantic retention after reload/cancellation; stale deferred entries must not survive forever.
        control.retained.clear();
        control.deferred.clear();
        for (auto const& [id, objective] : book.All())
            if (objective.quest && objective.state != ObjectiveState::Completed
                && objective.state != ObjectiveState::Cancelled)
            {
                control.retained.insert(objective.quest);
                if (objective.state == ObjectiveState::Deferred)
                    control.deferred.insert(objective.quest);
            }
        ProcessAdvice(owner, state, now, realMs);
        bool const waitingForInvitation = std::any_of(book.All().begin(), book.All().end(), [&](auto const& item)
        {
            auto const& cooperation = item.second.cooperation;
            return (cooperation.state == CooperationState::Recruiting || cooperation.state == CooperationState::Agreed)
                && now < cooperation.deadlineMs;
        });
        if (waitingForInvitation && CurrentControlMode(*bot) == ControlMode::AutonomousSolo)
        {
            if (auto const* current = book.Current())
            {
                Release(bot, current->id);
                book.Suspend(current->id, ObjectiveStep::Wait, "Waiting at the agreed rendezvous", now);
            }
            control.plannerAttached = true;
            state.availability = "waiting_for_matching_cooperative_invitation";
            Publish(owner, state, realMs);
            return;
        }
        if (CurrentControlMode(*bot) == ControlMode::AutonomousParty
            && (!state.partyQuest || bot->GetQuestRewardStatus(state.partyQuest)))
        {
            // Agreement alone is not readiness, and the leader's reward cannot stand for every member's reward.
            if (auto const* current = book.Current())
            {
                Release(bot, current->id);
                book.Suspend(current->id, ObjectiveStep::Wait, "Waiting for the agreed party to be ready", now);
            }
            control.plannerAttached = true;
            control.cooperationHold = true;
            state.availability = "waiting_for_cooperative_membership_readiness_or_peer_completion";
            Publish(owner, state, realMs);
            return;
        }
        if (state.partyQuest)
            if (auto const* current = book.Current(); current && current->quest != state.partyQuest)
            {
                auto const id = current->id;
                Release(bot, id);
                if (book.Block(id, Obstruction::Executor, "Honor the accepted cooperative quest first", now))
                    book.Defer(id, now);
            }
        state.knowledge.Seed(bot->getRace(), bot->getClass() == CLASS_DEATH_KNIGHT, startingComplete);
        if (state.currentArea != bot->GetAreaId())
            if (auto const* area = sAreaTableStore.LookupEntry(bot->GetAreaId()))
            {
                auto const known = state.knowledge.Places().find(area->ID);
                bool const firstVisit = known == state.knowledge.Places().end() || !known->second.visitedMs;
                if (state.knowledge.Visit(area->ID, PlayerbotAI::GetLocalizedAreaName(area), now, false))
                {
                    state.currentArea = area->ID;
                    if (firstVisit)
                        state.discoveredArea = area->ID;
                }
            }
        state.availability = "new_rpg";
        control.plannerAttached = true;
        auto const quests = QuestLog(*bot);
        uint32_t newQuest = 0;
        for (auto const quest : quests)
            if (!state.observedQuests.contains(quest))
            {
                auto const* definition = sObjectMgr->GetQuestTemplate(quest);
                if (definition && !definition->IsRepeatable() && bot->GetQuestStatus(quest) != QUEST_STATUS_FAILED)
                {
                    newQuest = quest;
                    break;
                }
            }
        state.observedQuests = quests;
        if (auto const* current = book.Current(); current && current->quest)
            book.ObserveOpportunity(current->id, VisibleQuestOpportunity(*bot, *ai, current->quest), now);
        // Reconcile all retained intentions, including reward/abandonment while deferred or under human control.
        for (auto const& [id, objective] : book.All())
        {
            if (objective.request || objective.state == ObjectiveState::Completed
                || objective.state == ObjectiveState::Cancelled)
                continue;
            if (!objective.quest)
            {
                if (objective.purpose != PlacePurpose::Work)
                    continue; // The purpose-specific executor supplies its own observation below.
                auto const step = control.token == id && !bot->IsInCombat() ? Step(*bot, control) : ObjectiveStep::Wait;
                book.ObservePlace(id, state.currentArea, newQuest, step, now);
                if (newQuest && objective.state == ObjectiveState::Active && state.currentArea != objective.place)
                    book.Cancel(id, "Useful work was accepted before reaching the intended area");
                if (objective.state == ObjectiveState::Completed || objective.state == ObjectiveState::Cancelled)
                {
                    Release(bot, id);
                    state.knowledge.RecordUsefulWork(state.currentArea, now, uint8_t(bot->GetLevel()));
                    AssessReports(state, state.currentArea, true);
                }
                continue;
            }
            auto const observed = Sample(*bot, objective.quest);
            auto const creditBefore = objective.gainedCredit;
            auto const step = objective.obstruction == Obstruction::Competition ? ObjectiveStep::Wait
                : control.Owns(objective.quest) ? Step(*bot, control) : ObjectiveStep::Wait;
            book.Observe(id, observed, step, now);
            if (objective.gainedCredit != creditBefore || objective.state == ObjectiveState::Completed)
            {
                state.knowledge.RecordUsefulWork(state.currentArea, now, uint8_t(bot->GetLevel()));
                AssessReports(state, state.currentArea, true);
            }
            if (objective.state == ObjectiveState::Completed || objective.state == ObjectiveState::Cancelled)
            {
                Release(bot, id);
                control.retained.erase(objective.quest);
                control.deferred.erase(objective.quest);
            }
        }
        if (!bot->GetMap()->Instanceable() && state.currentArea == bot->GetAreaId())
            if (auto* repairer = VisibleRepairer(*bot);
                repairer && bot->GetNPCIfCanInteractWith(repairer->GetGUID(), UNIT_NPC_FLAG_REPAIR))
                state.knowledge.RememberRepair(state.currentArea, {bot->GetMapId(), bot->GetPhaseMask(),
                    bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), now});
        ObserveSatisfaction(owner, state, now, realMs);
        PrepareCandidates(state, *bot, now);
        AssessActivities(state, *bot, *ai, now);
        RefreshMerchant(owner, state, *bot, now);
        ProcessDecision(owner, state, *bot, now, realMs);
        if (PrepareSupplies(owner, state, *bot, *ai, now, realMs))
            return;
        if (PrepareEquipment(owner, state, *bot, *ai, now, realMs))
            return;
        auto const* current = book.Current();
        if (brain && current && !state.partyQuest && CanPrepareResources(*bot)
            && state.satisfactionDecision.selected != current->id
            && book.Replan(current->id, "Another feasible activity offers greater expected satisfaction", now))
        {
            Release(bot, current->id);
            state.activeRoute.clear();
            state.routeIndex = 0;
            state.nextDecisionMs = now;
            current = nullptr;
        }
        if (current && !current->quest && current->purpose == PlacePurpose::Work && control.token == current->id)
        {
            bool const investigating = current->state == ObjectiveState::Active
                && current->step == ObjectiveStep::Attempt && current->place == state.currentArea
                && !bot->IsInCombat();
            state.survey.Observe(now, control.scans, control.lastScanEmpty, investigating,
                bot->GetPositionX(), bot->GetPositionY());
            if (investigating && state.survey.Exhausted())
            {
                state.knowledge.Investigate(current->place);
                AssessReports(state, current->place, false);
                book.Block(current->id, Obstruction::Information,
                    "Repeated local searches from different positions found no useful work; try another known area",
                    now);
            }
            else if (brain && investigating && state.survey.Stalled())
                book.Block(current->id, Obstruction::Navigation,
                    "Local searches could not cover distinct positions; no conclusion about available work", now);
        }
        if (!brain && current && current->state == ObjectiveState::Active && current->step == ObjectiveStep::Travel)
        {
            auto const* task = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
            auto const* travel = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data);
            WorldPosition const target = task && task->questId == current->quest ? task->pos
                : travel && !current->quest ? travel->pos : WorldPosition();
            if (target != WorldPosition())
            {
                float const distance = bot->GetDistance(target);
                if (target != state.routeTarget || distance + 5.0f < state.nearestRouteDistance)
                {
                    state.routeTarget = target;
                    state.nearestRouteDistance = distance;
                    state.lastRouteProgressMs = now;
                }
                else if (now >= state.lastRouteProgressMs && now - state.lastRouteProgressMs >= 90000)
                    book.Block(current->id, Obstruction::Navigation,
                        "No measured route advancement for 90 seconds", now);
            }
        }
        else
            state.routeTarget = WorldPosition();
        if (current && control.token == current->id && control.failure != QuestObjectiveControl::Failure::None)
        {
            if (brain && !current->quest && control.failure == QuestObjectiveControl::Failure::Navigation)
                if (auto const* leg = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data))
                {
                    state.failedRoutes.push_back(leg->pos);
                    if (state.failedRoutes.size() < 3)
                        if (auto const next = ResolveArea(*bot, current->place, state.failedRoutes);
                            next != WorldPosition())
                        {
                            control.failure = QuestObjectiveControl::Failure::None;
                            ai->rpgInfo.ChangeToGoCamp(next);
                            ai->rpgInfo.bodyTravel = {};
                            ai->rpgInfo.body.state = BodyControl::State::Running;
                            if (recorder)
                                recorder->Record(owner, "alles_body", current->id, "alternate_route",
                                    "Retry the same known area through another destination anchor", realMs);
                        }
                }
        }
        if (current && control.token == current->id && control.failure != QuestObjectiveControl::Failure::None)
        {
            bool const missing = control.failure == QuestObjectiveControl::Failure::MissingLocation;
            bool const navigation = control.failure == QuestObjectiveControl::Failure::Navigation;
            book.Block(current->id, missing ? Obstruction::Information
                : navigation ? Obstruction::Navigation : Obstruction::Executor,
                missing ? "Executor has no usable quest location"
                : navigation ? "Navigation made no route progress" : "Quest executor reported failure", now);
        }
        if (current && current->state == ObjectiveState::Blocked)
        {
            auto const quest = current->quest;
            Release(bot, current->id);
            book.Defer(current->id, now);
            if (quest)
                control.deferred.insert(quest);
            state.nextDecisionMs = now + 5000;
            current = nullptr;
        }
        if (current && (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInFlight()))
        {
            Release(bot, current->id);
            book.Suspend(current->id, bot->IsAlive() ? ObjectiveStep::Wait : ObjectiveStep::Recover,
                bot->IsAlive() ? "Travel handoff; reconcile on arrival" : "Recovering after death", now);
        }
        else if (!bot->IsInCombat() && bot->IsAlive() && !bot->IsBeingTeleported() && !bot->IsInFlight())
        {
            if (!current && now >= state.nextDecisionMs)
            {
                state.nextDecisionMs = now + 5000;
                if (brain)
                {
                    auto const* chosen = book.Find(state.satisfactionDecision.selected);
                    if (chosen && (!state.partyQuest || chosen->quest == state.partyQuest))
                    {
                        bool const activated = chosen->quest
                            ? ReadyToAttempt(*bot, chosen->quest, control)
                                && book.Activate(chosen->id, chosen->revision, Sample(*bot, chosen->quest),
                                    now, Circumstances(*bot))
                            : book.ActivatePlace(chosen->id, chosen->revision, now, Circumstances(*bot));
                        if (activated)
                        {
                            current = book.Current();
                            state.intention = current->id;
                            state.intentionSinceMs = now;
                            state.activityObservedMs = state.activityArrivalObservedMs = 0;
                            state.activeRoute = state.routes[current->id];
                            state.routeIndex = 0;
                            state.availability = "pursuing_satisfaction";
                        }
                    }
                    else
                        state.availability = "remaining_here_for_satisfaction";
                }
                else
                {
                    // Legacy memory-only mode retains its original work ordering.
                    for (bool reward : {true, false})
                    {
                        if (!reward && !current && !state.partyQuest)
                            for (auto const id : IncomeQuestOrder(book, OwnQuestFinances(*bot),
                                Circumstances(*bot), now))
                            {
                                auto const* candidate = book.Find(id);
                                if (ReadyToAttempt(*bot, candidate->quest, control)
                                    && book.Activate(id, candidate->revision, Sample(*bot, candidate->quest),
                                        now, Circumstances(*bot)))
                                {
                                    current = book.Current();
                                    book.Prefer(id, current->revision,
                                        "Earn own funds through this accepted quest before retrying an unpaid turn-in",
                                        0, now);
                                    if (recorder)
                                        recorder->Record(owner, "alles_plan", id, "income_quest_selected",
                                            "Feasible accepted paying work may fund a retained money-blocked turn-in; "
                                            "the reward has not been earned yet", realMs);
                                    break;
                                }
                            }
                        if (!reward && !current)
                            if (auto const* preferred = book.Preferred(now, Circumstances(*bot));
                                preferred && (!state.partyQuest || preferred->quest == state.partyQuest))
                            {
                                if (preferred->quest && ReadyToAttempt(*bot, preferred->quest, control))
                                    book.Activate(preferred->id, preferred->revision, Sample(*bot, preferred->quest),
                                        now, Circumstances(*bot));
                                else if (!preferred->quest)
                                    book.ActivatePlace(preferred->id, preferred->revision, now, Circumstances(*bot));
                                current = book.Current();
                            }
                        if (!reward && !current && !state.partyQuest)
                            for (auto const& [id, candidate] : book.All())
                                if (!candidate.quest && !candidate.evidence.empty()
                                    && book.ActivatePlace(id, candidate.revision, now, Circumstances(*bot)))
                                {
                                    current = book.Current();
                                    break;
                                }
                        for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE && !current; ++slot)
                        {
                            auto const quest = bot->GetQuestSlotQuestId(slot);
                            if (!quest || (state.partyQuest && quest != state.partyQuest)
                                || (bot->GetQuestStatus(quest) == QUEST_STATUS_COMPLETE) != reward)
                                continue;
                            auto const* definition = sObjectMgr->GetQuestTemplate(quest);
                            if (!definition || definition->IsRepeatable() || !ReadyToAttempt(*bot, quest, control))
                                continue;
                            auto const* candidate = book.ProposeQuest(quest,
                                "Earn the reward for " + definition->GetTitle(),
                                reward ? "An outstanding turn-in is useful work" : "Accepted quest in my own log");
                            if (candidate && book.Activate(candidate->id, candidate->revision, Sample(*bot, quest),
                                now, Circumstances(*bot)))
                                current = book.Current();
                        }
                    }
                    if (!current && !state.partyQuest)
                    {
                        auto choices = state.knowledge.Alternatives(state.currentArea, uint8_t(bot->GetLevel()));
                        std::sort(choices.begin(), choices.end(), [](auto const* left, auto const* right)
                        {
                            if (left->investigations != right->investigations)
                                return left->investigations < right->investigations;
                            if (left->lastUsefulWorkMs != right->lastUsefulWorkMs)
                                return left->lastUsefulWorkMs > right->lastUsefulWorkMs;
                            if (left->minimumLevel != right->minimumLevel)
                                return left->minimumLevel > right->minimumLevel;
                            return left->visitedMs < right->visitedMs;
                        });
                        auto const local = state.knowledge.Places().find(state.currentArea);
                        if (local != state.knowledge.Places().end())
                            choices.insert(choices.begin(), &local->second);
                        for (auto const* place : choices)
                        {
                            auto const* candidate = book.ProposePlace(place->area, "Discover work in " + place->name,
                                place->area == state.currentArea ? "Investigate local opportunities before leaving"
                                    : "Try a suitable place from my private geography knowledge");
                            if (candidate && book.ActivatePlace(candidate->id, candidate->revision,
                                now, Circumstances(*bot)))
                            {
                                current = book.Current();
                                break;
                            }
                        }
                        if (!current)
                            state.availability = "waiting_for_new_information_or_retry";
                    }
                }
            }
            if (current && current->state == ObjectiveState::Waiting
                && current->obstruction != Obstruction::Competition)
            {
                if (current->quest && ReadyToAttempt(*bot, current->quest, control))
                    book.Activate(current->id, current->revision, Sample(*bot, current->quest),
                        now, Circumstances(*bot));
                else if (!current->quest)
                    book.ActivatePlace(current->id, current->revision, now, Circumstances(*bot));
            }
            if (current && !control.token && (current->state == ObjectiveState::Active
                || (current->state == ObjectiveState::Waiting && current->obstruction == Obstruction::Competition)))
            {
                CapabilityContext context{owner, status->generation, current->id, current->revision,
                    true, {}, {}, {}};
                context.quests = quests;
                for (auto const& [area, place] : state.knowledge.Places())
                    context.places.insert(area);
                for (auto const& [person, contact] : state.knowledge.Contacts())
                    context.people.insert(person);
                CapabilityRequest request{1, owner, status->generation, current->id, current->revision,
                    current->approach, current->quest, current->place, current->person};
                if (capabilities.Validate(request, context).empty())
                {
                    if (current->quest)
                    {
                        auto const* definition = sObjectMgr->GetQuestTemplate(current->quest);
                        if (definition && control.Claim(current->id, current->quest))
                            ai->rpgInfo.ChangeToDoQuest(current->quest, definition);
                    }
                    else if (control.ClaimPlace(current->id, current->place))
                    {
                        if (current->purpose != PlacePurpose::Work)
                        {
                            state.survey = {};
                            ai->rpgInfo.ChangeToIdle();
                        }
                        else
                        {
                            state.failedRoutes.clear();
                            if (auto const known = state.knowledge.Places().find(state.currentArea);
                                known != state.knowledge.Places().end())
                                state.knowledge.Visit(known->first, known->second.name, now, false);
                            state.survey = {};
                            ai->rpgInfo.ChangeToIdle();
                            if (state.currentArea == current->place)
                                ai->rpgInfo.ChangeToWanderNpc();
                            else
                            {
                                auto const target = brain && !state.activeRoute.empty()
                                    ? state.activeRoute.front() : ResolveArea(*bot, current->place);
                                if (target != WorldPosition())
                                    ai->rpgInfo.ChangeToGoCamp(target);
                                else
                                    control.Fail(QuestObjectiveControl::Failure::MissingLocation);
                            }
                        }
                    }
                }
                else
                    book.Block(current->id, Obstruction::Information,
                        "The capability or its reference is no longer available", now);
            }
            else if (current && control.token == current->id && current->purpose == PlacePurpose::Work)
            {
                auto const* task = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
                bool const placeTask = !current->quest && (std::holds_alternative<NewRpgInfo::GoCamp>(ai->rpgInfo.data)
                    || std::holds_alternative<NewRpgInfo::WanderNpc>(ai->rpgInfo.data));
                if ((!task || task->questId != current->quest) && !placeTask)
                {
                    Release(bot, current->id);
                    book.Block(current->id, Obstruction::Executor,
                        "Execution changed outside objective ownership", now);
                }
                else if (placeTask && current->place != state.currentArea
                    && std::holds_alternative<NewRpgInfo::WanderNpc>(ai->rpgInfo.data))
                {
                    if (brain && state.routeIndex + 1 < state.activeRoute.size()
                        && bot->GetExactDist(state.activeRoute[state.routeIndex]) < 5)
                        ++state.routeIndex;
                    auto const target = brain && state.routeIndex < state.activeRoute.size()
                        ? state.activeRoute[state.routeIndex] : ResolveArea(*bot, current->place);
                    state.survey = {};
                    if (target != WorldPosition())
                        ai->rpgInfo.ChangeToGoCamp(target);
                    else
                        control.Fail(QuestObjectiveControl::Failure::MissingLocation);
                }
            }
        }
        if (current && brain && current->purpose != PlacePurpose::Work && control.token == current->id)
            ExecuteActivity(state, *bot, *ai, now);
        if (!brain && CurrentControlMode(*bot) == ControlMode::AutonomousSolo)
            if (auto const* earning = book.Current(); earning && earning->quest)
            {
                auto const income = IncomeQuestOrder(book, OwnQuestFinances(*bot), Circumstances(*bot), now);
                if (std::find(income.begin(), income.end(), earning->id) != income.end())
                    state.availability = "earning_quest_money";
            }
        QueueDecision(owner, state, *bot, now, realMs);
        if (!state.decision)
            AskForInformation(owner, state, *bot, now, realMs);
        if (!Publish(owner, state, realMs))
            if (auto const* current = book.Current())
            {
                Release(bot, current->id);
                book.Suspend(current->id, ObjectiveStep::Wait, "Planning persistence requires reconciliation", now);
            }
    }

    ActorStore& store;
    CapabilityRegistry capabilities;
    Telemetry::Recorder* recorder;
    ConversationRuntime* conversation;
    Bridge::Service* bridge;
    bool autonomousPlanning;
    bool brain;
    std::optional<WaveEmission> wave;
    std::optional<MerchantReceipt> merchantReceipt;
    uint64_t nextAdviceId = 0;
    std::map<ActorKey, Owner> states;
    uint64_t nextSampleMs = 0;
};

ObjectiveRuntime::ObjectiveRuntime(ActorStore& store, std::set<ActorKey> owners, Telemetry::Recorder* recorder,
    ConversationRuntime* conversation, Bridge::Service* bridge, bool autonomousPlanning, bool brain)
    : _impl(std::make_unique<Impl>(store, owners, recorder, conversation, bridge, autonomousPlanning, brain))
{
    if (conversation)
        conversation->SetObjectives(this);
}
ObjectiveRuntime::~ObjectiveRuntime()
{
    if (_impl->conversation)
        _impl->conversation->SetObjectives(nullptr);
}

boost::json::object ObjectiveRuntime::HelpOfferContext(ActorKey owner, uint64_t generation,
    RecruitmentNotice const& notice, uint64_t gameMs, uint64_t realMs) const
{
    auto progress = _impl->HelpEligibility(owner, generation, notice, gameMs, realMs);
    return {{"canOfferHelp", bool(progress)}, {"quest", notice.question.questName},
        {"meetingPlace", notice.question.placeName}, {"ownQuestAccepted", progress && progress->inLog},
        {"reason", progress ? "Compatible with my own current work; normal invitation and readiness still required"
            : "My current eligibility, readiness or commitments prevent an offer"}};
}

bool ObjectiveRuntime::OfferHelp(ActorKey owner, uint64_t generation, RecruitmentNotice const& notice,
    std::string const& statement, uint64_t gameMs, uint64_t realMs)
{
    return _impl->OfferHelp(owner, generation, notice, statement, gameMs, realMs);
}

bool ObjectiveRuntime::ReceiveRoster(ActorKey owner, uint64_t generation, CooperativeRoster const& roster,
    Reference const& source, uint64_t gameMs, uint64_t realMs)
{
    return _impl->ReceiveRoster(owner, generation, roster, source, gameMs, realMs);
}

boost::json::object ObjectiveRuntime::HumanContext(ActorKey owner, uint64_t generation, ActorKey person) const
{
    return _impl->HumanContext(owner, generation, person);
}

std::string ObjectiveRuntime::ApplyHumanRequest(ActorKey owner, uint64_t generation, Reference const& source,
    std::string const& statement, std::string const& action, ObjectGuid const& threat, uint64_t gameMs, uint64_t realMs)
{
    return _impl->ApplyHumanRequest(owner, generation, source, statement, action, threat, gameMs, realMs);
}

void ObjectiveRuntime::RequestPacket(Player& receiver, WorldPacket const& packet)
{
    if (_impl->merchantReceipt && packet.GetOpcode() == SMSG_LIST_INVENTORY
        && receiver.GetGUID() == ObjectGuid(HighGuid::Player, uint32_t(_impl->merchantReceipt->owner.id)))
    {
        auto decoded = DecodeMerchantInventory(packet);
        if (decoded && decoded->vendor == _impl->merchantReceipt->vendor.GetRawValue())
            _impl->merchantReceipt->received = std::move(decoded);
    }
    if (_impl->wave && packet.GetOpcode() == SMSG_EMOTE && packet.size() == 12
        && receiver.GetGUID() == ObjectGuid(HighGuid::Player, uint32_t(_impl->wave->recipient.id))
        && packet.read<uint32>(0) == EMOTE_ONESHOT_WAVE
        && packet.read<uint64>(4) == ObjectGuid(HighGuid::Player, uint32_t(_impl->wave->owner.id)).GetRawValue())
        _impl->wave->delivered = true;
}

void ObjectiveRuntime::RequesterLeft(ActorKey person, uint64_t realMs)
{
    for (auto& [owner, state] : _impl->states)
        if (auto const* following = state.book.Following(); following && following->person == person)
        {
            state.book.Cancel(following->id, "The requesting player left the world");
            _impl->ReleaseFollow(owner, state);
            _impl->Publish(owner, state, realMs);
        }
}

std::size_t ObjectiveRuntime::FollowingCount() const
{
    return std::count_if(_impl->states.begin(), _impl->states.end(),
        [](auto const& entry) { return entry.second.book.Following() != nullptr; });
}

void ObjectiveRuntime::Update(uint64_t gameMs, uint64_t realMs)
{
    if (gameMs < _impl->nextSampleMs)
        return;
    _impl->nextSampleMs = gameMs + 1000;
    if (_impl->bridge)
        for (auto& result : _impl->bridge->TakePlanning())
            for (auto& [owner, state] : _impl->states)
            {
                if (state.advice && state.advice->id == result.id)
                {
                    state.advice->result = std::move(result);
                    break;
                }
                else if (state.decision && state.decision->id == result.id)
                {
                    state.decision->result = std::move(result);
                    break;
                }
            }
    for (auto& [owner, state] : _impl->states)
    {
        if (_impl->brain)
            if (auto* bot = Find(owner))
                if (auto* ai = sPlayerbotsMgr.GetPlayerbotAI(bot); ai && ai->rpgInfo.body.Attached())
                    _impl->ObserveBody(owner, state, *ai, realMs);
        _impl->Tick(owner, state, gameMs, realMs);
        _impl->SyncBody(owner, state, realMs);
    }
}

void ObjectiveRuntime::Detach(ActorKey owner, uint64_t gameMs, uint64_t realMs)
{
    _impl->Detach(owner, gameMs, realMs);
}

void ObjectiveRuntime::Stop(uint64_t gameMs, uint64_t realMs)
{
    for (auto const& [owner, state] : _impl->states)
        _impl->Detach(owner, gameMs, realMs);
}

bool ObjectiveRuntime::SetMotive(ActorKey owner, std::string id, double weight, double depletion, double satiation,
    uint64_t realMs)
{
    auto found = _impl->states.find(owner);
    if (found == _impl->states.end() || !found->second.generation || !_impl->brain)
        return false;
    auto& state = found->second;
    auto staged = state.satisfaction;
    auto const known = staged.Capture().dimensions.find(id);
    double const fulfillment = known == staged.Capture().dimensions.end() ? 0 : known->second.fulfillment;
    if (!staged.SetDimension(std::move(id), {weight, fulfillment, depletion, satiation}))
        return false;
    auto previous = state.satisfaction;
    state.satisfaction = std::move(staged);
    if (!_impl->Publish(owner, state, realMs))
    {
        state.satisfaction = std::move(previous);
        return false;
    }
    state.nextAssessmentMs = state.lastPlannedSignal = 0;
    return true;
}

bool ObjectiveRuntime::SetEffect(ActorKey owner, std::string activity, std::string motive,
    double effect, uint64_t realMs)
{
    auto found = _impl->states.find(owner);
    if (found == _impl->states.end() || !found->second.generation || !_impl->brain)
        return false;
    auto& state = found->second;
    // Author effects only for actual observation adapters. An invented activity name cannot create new actions.
    if (!DefaultSatisfaction().activities.contains(activity))
        return false;
    auto staged = state.satisfaction;
    auto effects = staged.Effects(activity);
    effects[std::move(motive)] = effect;
    if (!staged.SetActivity(std::move(activity), std::move(effects)))
        return false;
    auto previous = state.satisfaction;
    state.satisfaction = std::move(staged);
    if (!_impl->Publish(owner, state, realMs))
    {
        state.satisfaction = std::move(previous);
        return false;
    }
    state.nextAssessmentMs = state.lastPlannedSignal = 0;
    return true;
}

boost::json::object ObjectiveRuntime::Status(ActorKey owner) const
{
    auto found = _impl->states.find(owner);
    if (found == _impl->states.end())
        return {};
    boost::json::array objectives;
    for (auto const& [id, objective] : found->second.book.All())
        objectives.emplace_back(Describe(objective));
    boost::json::array places, reports;
    boost::json::array dimensions, alternatives;
    for (auto const& [id, dimension] : found->second.satisfaction.Capture().dimensions)
        dimensions.emplace_back(boost::json::object{{"id", id}, {"weight", dimension.weight},
            {"fulfillment", dimension.fulfillment}, {"depletionPerHour", dimension.depletionPerHour},
            {"satiation", dimension.satiation}});
    for (auto const& candidate : found->second.satisfactionDecision.alternatives)
    {
        boost::json::object contributions;
        for (auto const& [id, contribution] : candidate.value.contributions)
            contributions[id] = contribution;
        auto const travel = found->second.travelTimes.find(candidate.id);
        auto const risk = found->second.routeRisks.find(candidate.id);
        auto const reason = found->second.routeReasons.find(candidate.id);
        auto const route = found->second.routes.find(candidate.id);
        alternatives.emplace_back(boost::json::object{{"objective", candidate.id}, {"expected", candidate.value.total},
            {"travelMs", travel == found->second.travelTimes.end() ? 0 : travel->second},
            {"risk", risk == found->second.routeRisks.end() ? 0 : risk->second},
            {"route", reason == found->second.routeReasons.end() ? "unavailable" : reason->second},
            {"waypoints", route == found->second.routes.end() ? 0 : route->second.size()},
            {"contributions", std::move(contributions)}});
    }
    for (auto const& [id, place] : found->second.knowledge.Places())
        places.emplace_back(boost::json::object{{"area", id}, {"name", place.name},
            {"minimumLevel", place.minimumLevel}, {"maximumLevel", place.maximumLevel},
            {"direction", place.direction}, {"relativeTo", place.relativeTo},
            {"startingKnowledge", place.origin == KnowledgeOrigin::Starting}, {"visitedMs", place.visitedMs},
            {"lastUsefulWorkMs", place.lastUsefulWorkMs}, {"repairKnown", bool(place.repair)},
            {"repairObservedMs", place.repair ? place.repair->observedMs : 0}});
    for (auto const& [id, report] : found->second.knowledge.Reports())
        reports.emplace_back(boost::json::object{{"id", id}, {"source", report.source.name},
            {"place", report.topic.place}, {"text", report.text}, {"receivedMs", report.receivedMs},
            {"confidence", report.confidence}, {"usefulVisits", report.usefulVisits},
            {"unsuccessfulVisits", report.unsuccessfulVisits}});
    boost::json::object experiences, bindings, routeFailures;
    auto const& satisfaction = found->second.satisfaction.Capture();
    for (auto const& [activity, experience] : satisfaction.experiences)
        experiences[activity] = boost::json::object{{"samples", experience.samples},
            {"successes", experience.successes}, {"meanExecutionMs", experience.meanDurationMs}};
    for (auto const& [activity, effects] : satisfaction.activities)
    {
        boost::json::object values;
        for (auto const& [dimension, effect] : effects)
            values[dimension] = effect;
        bindings[activity] = std::move(values);
    }
    for (auto const& [id, reason] : found->second.routeReasons)
        if (!found->second.routes.contains(id))
            routeFailures[std::to_string(id)] = reason;
    return {{"engine", found->second.availability}, {"objectives", std::move(objectives)},
        {"satisfaction", boost::json::object{{"revision", found->second.satisfaction.Capture().revision},
            {"dimensions", std::move(dimensions)}, {"alternatives", std::move(alternatives)},
            {"activityEffects", std::move(bindings)}, {"experiences", std::move(experiences)},
            {"nextRestMs", satisfaction.nextRestMs}, {"nextSocialMs", satisfaction.nextSocialMs},
            {"routeLimitations", std::move(routeFailures)}, {"routeIndex", found->second.routeIndex},
            {"selectedObjective", found->second.satisfactionDecision.selected},
            {"staying", found->second.satisfactionDecision.staying}}},
        {"body", found->second.bodyStatus},
        {"survey", boost::json::object{{"activeMs", found->second.survey.ActiveMs()},
            {"emptyScans", found->second.survey.EmptyScans()}, {"positions", found->second.survey.Positions()}}},
        {"planningRevision", found->second.planningRevision}, {"seedVersion", found->second.knowledge.SeedVersion()},
        {"places", std::move(places)}, {"reports", std::move(reports)}};
}
}
