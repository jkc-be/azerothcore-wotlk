/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "ObjectivePlanning.h"
#include <boost/json.hpp>
#include <algorithm>

namespace Alles
{
namespace
{
bool Terminal(Objective const& objective)
{
    return objective.state == ObjectiveState::Completed || objective.state == ObjectiveState::Cancelled;
}

bool Pursuable(Objective const& objective, ObjectiveBook const& book, uint64_t now, uint64_t circumstances)
{
    return (objective.state == ObjectiveState::Proposed || objective.state == ObjectiveState::Active
        || objective.state == ObjectiveState::Waiting || book.Retryable(objective, now, circumstances))
        && (!objective.quest || (objective.checkpoint.inLog && !objective.checkpoint.failed
            && !objective.checkpoint.rewarded));
}

bool CanDefer(Objective const& objective)
{
    return objective.state == ObjectiveState::Active && objective.activeWithoutProgressMs >= 60000
        && !objective.checkpoint.readyToReward;
}

std::string Token(uint64_t id)
{
    return "objective-" + std::to_string(id);
}

bool Associated(LearnedReport const& report, Objective const& objective)
{
    return (objective.quest && report.topic.quest == objective.quest)
        || (objective.place && report.topic.place == objective.place);
}
}

uint64_t MotivationDecisionSignal(SatisfactionSnapshot const& satisfaction)
{
    uint64_t hash = 14695981039346656037ULL;
    for (auto const& [id, dimension] : satisfaction.dimensions)
    {
        for (unsigned char c : id)
            hash = (hash ^ c) * 1099511628211ULL;
        // Measured growth already enters equipment/finance signals. Needs use five meaningful bands.
        if (dimension.curve == MotivationCurve::Need)
            hash = (hash ^ uint64_t(dimension.fulfillment * 5)) * 1099511628211ULL;
    }
    for (auto const& [id, experience] : satisfaction.contexts)
        hash = (hash ^ experience.samples ^ experience.observedMs) * 1099511628211ULL;
    return hash;
}

bool PlanningCadence::Ready(uint64_t signal, uint64_t now)
{
    if (signal != seen)
    {
        seen = signal;
        sinceMs = now;
    }
    return signal != submitted && now >= nextMs && now >= sinceMs && now - sinceMs >= 2000;
}

void PlanningCadence::Submitted(uint64_t signal, uint64_t now)
{
    submitted = signal;
    nextMs = now + 10000;
}

CapabilityRegistry ObjectiveCapabilities()
{
    CapabilityRegistry registry;
    registry.Register({"none", false, false, false, "Existing execution or fallback is adequate",
        "Keep existing intentions", "No ongoing action"});
    registry.Register({"follow", false, false, true, "A nearby human explicitly requests bounded accompaniment",
        "Observe ordinary movement and proximity within the request window", "Stop, expire or yield to human control"});
    registry.Register({"stop", false, false, true, "The same human asks to end the retained accompaniment",
        "Release only the request's own following movement", "The request is cancelled"});
    registry.Register({"wave", false, false, true, "A nearby human requests a wave",
        "Observe normal emote delivery to the requesting player", "Do not replay an unobserved effect after reload"});
    registry.Register({"assist", false, false, true, "A nearby human requests help against the engaged creature",
        "Observe own engagement against that exact visible threat", "Never reacquire a lost target on reload"});
    registry.Register({"pursue_quest", true, false, false, "An accepted viable quest owned by this bot",
        "Prefer ordinary quest execution; observed counters and reward establish progress", "Release quest ownership"});
    registry.Register({"repair_equipment", true, false, false, "Critically damaged equipped items; no competing work",
        "Approach a visible repairer; sell expendable junk if needed; observe paid equipment recovery",
        "Bound preparation; retain quest"});
    registry.Register({"buy_quest_supplies", true, false, false, "A received merchant offer supplies a required item",
        "Recheck menu; sell expendable junk if needed; use actual own funds and observe inventory gain",
        "Bound the purchase; retain the quest"});
    registry.Register({"discover_work", false, true, false, "A privately known place suitable for investigation",
        "Prefer ordinary travel and investigation; arrival alone is not success", "Release investigation ownership"});
    registry.Register({"explore_place", false, true, false, "A privately known place with unobserved surroundings",
        "Travel normally and observe something personally new; no quest reward is required", "Release exploration"});
    registry.Register({"develop_skills", false, true, false, "A suitable practice opportunity is personally visible",
        "Use ordinary gameplay to gain experience; only observed progress completes the activity", "Release practice"});
    registry.Register({"rest", false, true, false, "A known place where stationary rest is currently feasible",
        "Observe stationary rest outside combat; an idle command alone is not recovery", "Resume on interruption"});
    registry.Register({"visit_companion", false, true, true, "A personally observed companion at a known place",
        "Travel to the observed meeting site and deliver an ordinary interaction to that person",
        "Reconsider if the person is absent; never follow hidden global coordinates"});
    registry.Register({"ask_quest_advice", true, false, false, "An eligible information-blocked accepted quest",
        "Attempt a bounded normal question; only a received usable answer changes the plan", "Expire the question"});
    registry.Register({"ask_place_advice", false, true, false, "An eligible information-blocked private place",
        "Attempt a bounded normal question; only a received usable answer changes the plan", "Expire the question"});
    registry.Register({"seek_companions", true, false, false, "An accepted quest deferred for lack of companions",
        "Ask for willing companions; observed agreement, invitation and readiness precede execution",
        "Expire recruitment or yield to human control"});
    registry.Register({"defer_quest", true, false, false, "An active quest has measured unproductive execution",
        "Retain the quest, release execution and set a retry window", "Reconcile on new progress or circumstances"});
    registry.Register({"defer_place", false, true, false, "Active investigation has measured unproductive execution",
        "Retain the intention, release execution and set a retry window", "Reconcile on new information"});
    return registry;
}

uint32_t MissingQuestMoney(ObjectiveBook const& book, QuestFinances const& finances)
{
    uint32_t missing = 0;
    for (auto const& [id, objective] : book.All())
        if (!Terminal(objective) && objective.checkpoint.inLog && !objective.checkpoint.failed
            && !objective.checkpoint.rewarded && finances.turnInCredit.contains(objective.quest))
            if (auto const found = finances.money.find(objective.quest);
                found != finances.money.end() && found->second < 0)
            {
                auto const required = uint32_t(-int64_t(found->second));
                if (required > finances.ownMoney)
                    missing = std::max(missing, required - finances.ownMoney);
            }
    return missing;
}

std::vector<uint64_t> IncomeQuestOrder(ObjectiveBook const& book, QuestFinances const& finances,
    uint64_t circumstances, uint64_t now)
{
    std::vector<uint64_t> result;
    auto const missing = MissingQuestMoney(book, finances);
    if (!missing)
        return result;
    for (auto const& [id, objective] : book.All())
        if (auto const reward = finances.money.find(objective.quest);
            reward != finances.money.end() && reward->second > 0 && Pursuable(objective, book, now, circumstances)
            && (objective.obstruction == Obstruction::None || book.Retryable(objective, now, circumstances)))
            result.push_back(id);
    std::stable_sort(result.begin(), result.end(), [&](uint64_t left, uint64_t right)
    {
        auto rank = [&](uint64_t id)
        {
            auto const* objective = book.Find(id);
            return std::tuple{objective->checkpoint.readyToReward,
                std::min(missing, uint32_t(finances.money.at(objective->quest))), objective->gainedCredit};
        };
        return rank(left) > rank(right);
    });
    return result;
}

uint64_t ObjectiveDecisionSignal(ObjectiveBook const& book, PrivateKnowledge const& knowledge,
    uint8_t level, uint32_t area, uint64_t circumstances, uint64_t now,
    std::optional<QuestFinances> const& finances)
{
    uint64_t signal = 14695981039346656037ULL;
    auto add = [&](uint64_t value) { signal = (signal ^ value) * 1099511628211ULL; };
    add(level);
    add(area);
    add(circumstances);
    if (finances)
    {
        add(finances->ownMoney);
        for (auto const& [quest, money] : finances->money)
        {
            add(quest);
            add(uint64_t(int64_t(money)));
            add(finances->turnInCredit.contains(quest));
        }
    }
    for (auto const& [id, objective] : book.All())
    {
        add(id);
        add(uint8_t(objective.state));
        add(uint8_t(objective.obstruction));
        add(objective.gainedCredit);
        add(objective.checkpoint.readyToReward);
        add(objective.checkpoint.inLog);
        add(objective.deaths);
        add(objective.attempts);
        add(book.Retryable(objective, now, circumstances));
        add(book.CanRepair(id, now));
        add(book.CanBuySupplies(id, now));
        if (objective.preparation)
            add(uint8_t(objective.preparation->state));
        add(uint8_t(objective.information.status));
        add(uint8_t(objective.cooperation.state));
        add(objective.cooperation.agreements.size());
        add(objective.activeWithoutProgressMs >= 60000);
    }
    for (auto const& [id, place] : knowledge.Places())
    {
        add(id);
        add(place.investigations);
        add(place.lastUsefulWorkMs);
    }
    for (auto const& [id, report] : knowledge.Reports())
    {
        add(id);
        add(report.usefulVisits);
        add(report.unsuccessfulVisits);
    }
    return signal;
}

std::optional<ObjectivePlanningJob> PrepareObjectivePlanning(ActorKey owner, uint64_t generation,
    ObjectiveBook const& book, PrivateKnowledge const& knowledge, uint8_t level, uint32_t area,
    uint64_t circumstances, bool canAsk, uint64_t now, std::optional<QuestFinances> const& finances,
    SatisfactionDecision const* satisfaction)
{
    if (!IsValidActor(owner) || !generation || !level || level > 80 || book.Preparing())
        return std::nullopt;
    std::vector<Objective const*> candidates;
    auto const income = finances ? IncomeQuestOrder(book, *finances, circumstances, now) : std::vector<uint64_t>{};
    auto const* current = book.Current();
    for (auto const& [id, objective] : book.All())
        if (!Terminal(objective) && (objective.quest || knowledge.Places().contains(objective.place))
            && (objective.purpose != PlacePurpose::Companionship
                || (objective.person && knowledge.Contacts().contains(*objective.person))))
            candidates.push_back(&objective);
    std::stable_sort(candidates.begin(), candidates.end(), [&](auto const* left, auto const* right)
    {
        if (satisfaction)
        {
            auto rank = [&](Objective const* objective)
            {
                auto const found = std::find_if(satisfaction->alternatives.begin(), satisfaction->alternatives.end(),
                    [&](auto const& value) { return value.id == objective->id; });
                return std::tuple{objective->id == satisfaction->selected,
                    found != satisfaction->alternatives.end(),
                    found == satisfaction->alternatives.end() ? 0.0 : found->value.total, objective == current};
            };
            return rank(left) > rank(right);
        }
        auto rank = [&](Objective const* value)
        {
            return std::tuple{value == current, value->checkpoint.readyToReward,
                std::find(income.begin(), income.end(), value->id) != income.end(),
                canAsk && book.CanAsk(value->id, now), value->plannedMs,
                Pursuable(*value, book, now, circumstances)};
        };
        return rank(left) > rank(right);
    });
    if (candidates.empty())
        return std::nullopt;
    auto const* anchor = candidates.front();
    if (candidates.size() > 8)
        candidates.resize(8);
    auto registry = ObjectiveCapabilities();
    ObjectivePlanningJob job;
    job.createdMs = now;
    if (satisfaction)
        job.satisfactionSelection = satisfaction->selected;
    job.issued = {owner, generation, anchor->id, anchor->revision, true, {}, {}, {}};
    auto encode = [&]()
    {
        job.options.clear();
        job.reports.clear();
        job.issued.quests.clear();
        job.issued.places.clear();
        job.issued.people.clear();
        boost::json::array quests, places, people, objectives, evidence, options, specs;
        std::set<std::string> available{"none"};
        for (auto const* objective : candidates)
        {
            auto add = [&](std::string name)
            {
                auto const person = registry.All().at(name).person ? objective->person : std::nullopt;
                job.options.push_back({name, objective->id, objective->revision, objective->quest,
                    objective->place, person});
                available.insert(name);
                boost::json::object option{{"capability", name}, {"quest", objective->quest},
                    {"place", objective->place}, {"person", person ? boost::json::value(boost::json::object{
                        {"kind", uint8_t(person->kind)}, {"id", person->id}}) : boost::json::value(nullptr)},
                    {"evidence", Token(objective->id)}};
                if (person && job.issued.people.insert(*person).second)
                    people.emplace_back(boost::json::object{{"kind", uint8_t(person->kind)}, {"id", person->id}});
                if (satisfaction)
                    for (auto const& assessed : satisfaction->alternatives)
                        if (assessed.id == objective->id)
                        {
                            boost::json::object contributions;
                            for (auto const& [dimension, value] : assessed.value.contributions)
                                contributions[dimension] = value;
                            option["satisfaction"] = boost::json::object{{"expected", assessed.value.total},
                                {"contributions", std::move(contributions)}};
                            break;
                        }
                options.emplace_back(std::move(option));
            };
            if (Pursuable(*objective, book, now, circumstances))
                add(objective->quest ? "pursue_quest" : ActivityCapability(objective->purpose));
            if (book.CanRepair(objective->id, now))
                add("repair_equipment");
            if (book.CanBuySupplies(objective->id, now))
                add("buy_quest_supplies");
            if (canAsk && book.CanAsk(objective->id, now))
                add(objective->obstruction == Obstruction::Companions ? "seek_companions"
                    : objective->quest ? "ask_quest_advice" : "ask_place_advice");
            if (CanDefer(*objective))
                add(objective->quest ? "defer_quest" : "defer_place");
            if (objective->quest)
            {
                job.issued.quests.insert(objective->quest);
                quests.emplace_back(boost::json::object{{"id", objective->quest}, {"outcome", objective->outcome}});
            }
            else
            {
                auto const& place = knowledge.Places().at(objective->place);
                job.issued.places.insert(place.area);
                places.emplace_back(boost::json::object{{"id", place.area}, {"name", place.name},
                    {"minimumLevel", place.minimumLevel}, {"maximumLevel", place.maximumLevel},
                    {"relativeTo", place.relativeTo}, {"direction", place.direction}, {"origin", uint8_t(place.origin)},
                    {"visitedMs", place.visitedMs}, {"usefulWorkMs", place.lastUsefulWorkMs},
                    {"investigations", place.investigations}});
            }
            objectives.emplace_back(boost::json::object{{"id", objective->id}, {"quest", objective->quest},
                {"place", objective->place}, {"state", Name(objective->state)}, {"step", Name(objective->step)},
                {"reason", objective->reason}, {"outcome", objective->outcome}, {"purpose", Name(objective->purpose)},
                {"obstruction", Name(objective->obstruction)}, {"gainedCredit", objective->gainedCredit},
                {"activeWithoutProgressMs", objective->activeWithoutProgressMs}, {"deaths", objective->deaths},
                {"plannedMs", objective->plannedMs}, {"nextReconsiderationMs", objective->nextReconsiderationMs},
                {"preparation", objective->preparation ? boost::json::value(boost::json::object{
                    {"state", Name(objective->preparation->state)}, {"kind", uint8_t(objective->preparation->kind)},
                    {"item", objective->preparation->item}, {"count", objective->preparation->count},
                    {"reason", objective->preparation->reason}}) : boost::json::value(nullptr)}});
            evidence.emplace_back(boost::json::object{{"token", Token(objective->id)},
                {"kind", "own_objective_state"}, {"objective", objective->id}});
        }
        for (auto const& name : available)
        {
            auto const& spec = registry.All().at(name);
            specs.emplace_back(boost::json::object{{"name", name}, {"quest", spec.quest}, {"place", spec.place},
                {"person", spec.person}, {"precondition", spec.precondition},
                {"observableEffect", spec.observableEffect}, {"cancellation", spec.cancellation}});
        }
        job.context = {{"purpose", "decision"}, {"gameTimeMs", now}, {"level", level}, {"currentArea", area},
            {"currentObjective", current ? current->id : 0}, {"objectives", std::move(objectives)},
            {"capabilities", std::move(specs)}, {"options", std::move(options)}, {"quests", std::move(quests)},
            {"places", std::move(places)}, {"people", std::move(people)}, {"evidence", std::move(evidence)}};
        if (satisfaction)
            job.context["satisfaction"] = boost::json::object{{"revision", satisfaction->stateRevision},
                {"selectedObjective", satisfaction->selected}, {"staying", satisfaction->staying},
                {"meaning", "Expected personal value over the actor's planning horizon, including "
                    "travel, uncertainty and "
                    "observed needs. Selection also accounts for the current commitment and switching threshold."}};
        if (finances)
        {
            boost::json::array amounts;
            for (auto const* objective : candidates)
                if (auto const found = finances->money.find(objective->quest); found != finances->money.end())
                    amounts.emplace_back(boost::json::object{{"quest", objective->quest}, {"money", found->second},
                        {"turnInCredit", finances->turnInCredit.contains(objective->quest)}});
            job.context["funds"] = boost::json::object{{"ownMoney", finances->ownMoney},
                {"missingForTurnIns", MissingQuestMoney(book, *finances)}, {"acceptedQuestMoney", std::move(amounts)},
                {"meaning", "Positive amounts are expected rewards, negative amounts required payment. "
                    "Only actual ownMoney can be spent. Feasible paying work may fund a retained blocked turn-in."}};
        }
    };
    encode();
    while (boost::json::serialize(job.context).size() > 7500 && candidates.size() > 1)
    {
        candidates.pop_back();
        encode();
    }
    // Optional source-attributed reports are bounded by actual serialized context capacity.
    for (auto it = knowledge.Reports().rbegin(); it != knowledge.Reports().rend() && job.reports.size() < 4; ++it)
    {
        auto const& [id, report] = *it;
        if (std::none_of(candidates.begin(), candidates.end(), [&](auto const* candidate)
            { return Associated(report, *candidate); }))
            continue;
        auto const token = "report-" + std::to_string(id);
        auto& evidence = job.context.at("evidence").as_array();
        evidence.emplace_back(boost::json::object{{"token", token}, {"kind", "uncertain_report"},
            {"quest", report.topic.quest}, {"place", report.topic.place}, {"source", report.source.name},
            {"text", report.text}, {"receivedMs", report.receivedMs}, {"confidence", report.confidence},
            {"usefulVisits", report.usefulVisits}, {"unsuccessfulVisits", report.unsuccessfulVisits}});
        if (boost::json::serialize(job.context).size() > 8192)
            evidence.pop_back();
        else
            job.reports.emplace(token, id);
    }
    if (job.options.empty() || boost::json::serialize(job.context).size() > 8192)
        return std::nullopt;
    return job;
}

ObjectiveChoice ApplyObjectiveChoice(ObjectivePlanningJob const& job, Bridge::PlanningDecision const& decision,
    CapabilityContext const& live, ObjectiveBook& book, PrivateKnowledge const& knowledge,
    uint64_t circumstances, bool canAsk, uint64_t now, SatisfactionDecision const* satisfaction)
{
    auto registry = ObjectiveCapabilities();
    if (auto const invalid = registry.Validate(decision.request, job.issued); !invalid.empty())
        return {invalid};
    if (auto const invalid = registry.Validate(decision.request, live); !invalid.empty())
        return {invalid};
    auto const* anchor = book.Find(job.issued.objective);
    if (!anchor || anchor->revision != job.issued.revision || now < job.createdMs || now - job.createdMs >= 45000)
        return {"stale_decision"};
    auto const& request = decision.request;
    if (request.capability == "none")
        return {decision.evidence.empty() ? "kept_current_plan" : "invalid_evidence"};
    auto option = std::find_if(job.options.begin(), job.options.end(), [&](auto const& value)
    {
        return value.capability == request.capability && value.quest == request.quest && value.place == request.place
            && value.person == request.person;
    });
    if (option == job.options.end())
        return {"option_not_supplied"};
    auto const* target = book.Find(option->objective);
    if (!target || target->revision != option->revision || Terminal(*target)
        || (target->place && !knowledge.Places().contains(target->place)))
        return {"stale_option"};
    uint64_t report = 0;
    if (decision.evidence != Token(target->id))
    {
        auto found = job.reports.find(decision.evidence);
        if (found == job.reports.end() || !knowledge.Reports().contains(found->second)
            || !Associated(knowledge.Reports().at(found->second), *target))
            return {"invalid_evidence"};
        report = found->second;
    }
    if (job.satisfactionSelection && (!satisfaction || satisfaction->selected != target->id
        || *job.satisfactionSelection != target->id))
        return {"satisfaction_preference_changed"};
    if (request.capability == "repair_equipment")
        return book.BeginRepair(target->id, target->revision, now)
            ? ObjectiveChoice{"equipment_preparation_started", target->id} : ObjectiveChoice{"preparation_unavailable"};
    if (request.capability == "buy_quest_supplies")
        return book.BeginSupplyPurchase(target->id, target->revision, now)
            ? ObjectiveChoice{"supplies_preparation_started", target->id} : ObjectiveChoice{"preparation_unavailable"};
    if (request.capability == "ask_quest_advice" || request.capability == "ask_place_advice"
        || request.capability == "seek_companions")
        return canAsk && book.CanAsk(target->id, now) ? ObjectiveChoice{"question_selected", 0, 0, target->id}
            : ObjectiveChoice{"question_unavailable"};
    if (request.capability == "defer_quest" || request.capability == "defer_place")
    {
        if (!CanDefer(*target))
            return {"deferral_precondition_changed"};
        auto const id = target->id;
        auto staged = book;
        if (!staged.Block(id, Obstruction::Executor, decision.reason, now) || !staged.Defer(id, now))
            return {"deferral_rejected"};
        book = std::move(staged);
        return {"deferred", 0, id};
    }
    if (!Pursuable(*target, book, now, circumstances))
        return {"work_no_longer_available"};
    if (auto const* preferred = book.Preferred(now, circumstances);
        preferred && preferred->id != target->id && now - preferred->plannedMs < 120000)
        return {"recent_commitment_held"};
    auto const id = target->id;
    if (!book.Prefer(id, target->revision, decision.reason, report, now))
        return {"preference_rejected"};
    return {"intention_preferred", id};
}
}
