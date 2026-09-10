/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "PlanningCodec.h"
#include "bridge/Wire.h"
#include <limits>
#include <stdexcept>

namespace Alles::Storage
{
namespace
{
using boost::json::array;
using boost::json::object;
using boost::json::value;
using Bridge::Fields;
using Bridge::String;

template <typename T>
T UInt(object const& object, char const* key)
{
    auto const number = Bridge::Number(object, key);
    if (number > std::numeric_limits<T>::max())
        throw std::invalid_argument("planning integer overflow");
    return static_cast<T>(number);
}

object Actor(ActorKey owner)
{
    return {{"kind", uint8_t(owner.kind)}, {"id", owner.id}};
}

object EncodeExperience(std::string const& id, SatisfactionExperience const& experience)
{
    auto encodeEffects = [](std::map<std::string, LearnedEffect> const& learned)
    {
        object effects;
        for (auto const& [dimension, effect] : learned)
            effects[dimension] = object{{"samples", effect.samples}, {"mean", effect.mean}};
        return effects;
    };
    return {{"id", id}, {"samples", experience.samples}, {"successes", experience.successes},
        {"meanDurationMs", experience.meanDurationMs}, {"effects", encodeEffects(experience.effects)},
        {"failureEffects", encodeEffects(experience.failureEffects)}, {"observedMs", experience.observedMs}};
}

SatisfactionExperience ReadExperience(object const& experience, uint32_t version)
{
    bool const modern = version >= 12;
    if (version >= 14)
        Fields(experience, {"id", "samples", "successes", "meanDurationMs", "effects", "failureEffects", "observedMs"});
    else if (modern)
        Fields(experience, {"id", "samples", "successes", "meanDurationMs", "effects", "failureEffects"});
    else
        Fields(experience, {"id", "samples", "successes", "meanDurationMs"});
    SatisfactionExperience result{UInt<uint32_t>(experience, "samples"), UInt<uint32_t>(experience, "successes"),
        experience.at("meanDurationMs").to_number<double>()};
    if (version >= 14)
        result.observedMs = UInt<uint64_t>(experience, "observedMs");
    if (modern)
    {
        auto readEffects = [](value const& data)
        {
            std::map<std::string, LearnedEffect> effects;
            auto const& values = data.as_object();
            if (values.size() > 32)
                throw std::invalid_argument("too many learned effects");
            for (auto const& entry : values)
            {
                auto const& effect = entry.value().as_object();
                Fields(effect, {"samples", "mean"});
                effects.emplace(std::string(entry.key()), LearnedEffect{UInt<uint32_t>(effect, "samples"),
                    effect.at("mean").to_number<double>()});
            }
            return effects;
        };
        result.effects = readEffects(experience.at("effects"));
        result.failureEffects = readEffects(experience.at("failureEffects"));
    }
    return result;
}

object EncodeSatisfaction(SatisfactionSnapshot const& state)
{
    array dimensions, activities, experiences, contexts;
    for (auto const& [id, dimension] : state.dimensions)
        dimensions.emplace_back(object{{"id", id}, {"weight", dimension.weight},
            {"fulfillment", dimension.fulfillment}, {"depletionPerHour", dimension.depletionPerHour},
            {"satiation", dimension.satiation}, {"curve", uint8_t(dimension.curve)}, {"scale", dimension.scale},
            {"urgency", dimension.urgency}});
    for (auto const& [id, effects] : state.activities)
    {
        object values;
        for (auto const& [dimension, effect] : effects)
            values[dimension] = effect;
        activities.emplace_back(object{{"id", id}, {"effects", std::move(values)}});
    }
    for (auto const& [id, experience] : state.experiences)
        experiences.emplace_back(EncodeExperience(id, experience));
    for (auto const& [id, experience] : state.contexts)
        contexts.emplace_back(EncodeExperience(id, experience));
    array travel;
    for (auto const& [route, experience] : state.travel)
        travel.emplace_back(object{{"route", route}, {"samples", experience.samples},
            {"successes", experience.successes}, {"durationRatio", experience.durationRatio}});
    return {{"revision", state.revision}, {"observedMs", state.observedMs}, {"dimensions", std::move(dimensions)},
        {"activities", std::move(activities)}, {"experiences", std::move(experiences)},
        {"nextRestMs", state.nextRestMs}, {"nextSocialMs", state.nextSocialMs}, {"travel", std::move(travel)},
        {"contexts", std::move(contexts)}, {"horizonMs", state.horizonMs}};
}

SatisfactionSnapshot ReadSatisfaction(object const& object, uint32_t version)
{
    bool const modern = version >= 12;
    if (modern)
        Fields(object, {"revision", "observedMs", "dimensions", "activities", "experiences",
            "nextRestMs", "nextSocialMs", "travel", "contexts", "horizonMs"});
    else if (object.contains("travel"))
        Fields(object, {"revision", "observedMs", "dimensions", "activities", "experiences",
            "nextRestMs", "nextSocialMs", "travel"});
    else
        Fields(object, {"revision", "observedMs", "dimensions", "activities", "experiences",
            "nextRestMs", "nextSocialMs"});
    SatisfactionSnapshot result;
    result.revision = UInt<uint64_t>(object, "revision");
    result.observedMs = UInt<uint64_t>(object, "observedMs");
    result.nextRestMs = UInt<uint64_t>(object, "nextRestMs");
    result.nextSocialMs = UInt<uint64_t>(object, "nextSocialMs");
    auto const& dimensions = object.at("dimensions").as_array();
    if (dimensions.empty() || dimensions.size() > 32)
        throw std::invalid_argument("invalid satisfaction dimensions");
    for (auto const& value : dimensions)
    {
        auto const& dimension = value.as_object();
        if (version >= 13)
            Fields(dimension, {"id", "weight", "fulfillment", "depletionPerHour", "satiation",
                "curve", "scale", "urgency"});
        else if (modern)
            Fields(dimension, {"id", "weight", "fulfillment", "depletionPerHour", "satiation", "curve", "scale"});
        else
            Fields(dimension, {"id", "weight", "fulfillment", "depletionPerHour", "satiation"});
        SatisfactionDimension item{dimension.at("weight").to_number<double>(),
            dimension.at("fulfillment").to_number<double>(), dimension.at("depletionPerHour").to_number<double>(),
            dimension.at("satiation").to_number<double>()};
        if (modern)
        {
            item.curve = MotivationCurve(UInt<uint8_t>(dimension, "curve"));
            item.scale = dimension.at("scale").to_number<double>();
        }
        if (version >= 13)
            item.urgency = dimension.at("urgency").to_number<double>();
        if (!result.dimensions.emplace(String(dimension, "id", 32), item).second)
            throw std::invalid_argument("duplicate satisfaction dimension");
    }
    auto const& activities = object.at("activities").as_array();
    auto const& experiences = object.at("experiences").as_array();
    if (activities.size() > 32 || experiences.size() > 32)
        throw std::invalid_argument("too many satisfaction activities");
    for (auto const& value : activities)
    {
        auto const& activity = value.as_object();
        Fields(activity, {"id", "effects"});
        auto const& values = activity.at("effects").as_object();
        if (values.size() > 32)
            throw std::invalid_argument("too many satisfaction effects");
        SatisfactionEffects effects;
        for (auto const& effect : values)
            effects.emplace(std::string(effect.key()), effect.value().to_number<double>());
        if (!result.activities.emplace(String(activity, "id", 32), std::move(effects)).second)
            throw std::invalid_argument("duplicate satisfaction activity");
    }
    for (auto const& value : experiences)
    {
        auto const& experience = value.as_object();
        auto item = ReadExperience(experience, version);
        if (version < 14)
            item.observedMs = result.observedMs;
        if (!result.experiences.emplace(String(experience, "id", 32), item).second)
            throw std::invalid_argument("duplicate satisfaction experience");
    }
    if (auto const* travel = object.if_contains("travel"))
    {
        if (travel->as_array().size() > 32)
            throw std::invalid_argument("too many travel experiences");
        for (auto const& value : travel->as_array())
        {
            auto const& experience = value.as_object();
            Fields(experience, {"route", "samples", "successes", "durationRatio"});
            TravelExperience item{UInt<uint32_t>(experience, "samples"), UInt<uint32_t>(experience, "successes"),
                experience.at("durationRatio").to_number<double>()};
            if (!result.travel.emplace(String(experience, "route", 32), item).second)
                throw std::invalid_argument("duplicate travel experience");
        }
    }
    if (modern)
    {
        result.horizonMs = UInt<uint64_t>(object, "horizonMs");
        auto const& contexts = object.at("contexts").as_array();
        if (contexts.size() > 128)
            throw std::invalid_argument("too many learned contexts");
        for (auto const& value : contexts)
        {
            auto const& experience = value.as_object();
            auto item = ReadExperience(experience, version);
            if (version < 14)
                item.observedMs = result.observedMs;
            if (!result.contexts.emplace(String(experience, "id", 65), item).second)
                throw std::invalid_argument("duplicate learned context");
        }
    }
    if (!IsValidSatisfaction(result))
        throw std::invalid_argument("invalid satisfaction state");
    return result;
}

value MaybeActor(std::optional<ActorKey> actor)
{
    return actor ? value(Actor(*actor)) : value(nullptr);
}

ActorKey ReadActor(value const& value)
{
    auto const& object = value.as_object();
    Fields(object, {"kind", "id"});
    ActorKey actor{ActorKind(UInt<uint8_t>(object, "kind")), UInt<uint64_t>(object, "id")};
    if (!IsValidActor(actor))
        throw std::invalid_argument("invalid planning actor");
    return actor;
}

std::optional<ActorKey> ReadMaybeActor(value const& value)
{
    return value.is_null() ? std::nullopt : std::optional(ReadActor(value));
}

object Progress(QuestProgress const& progress)
{
    array counters;
    for (auto count : progress.counters)
        counters.push_back(count);
    return {{"counters", std::move(counters)}, {"inLog", progress.inLog},
        {"readyToReward", progress.readyToReward}, {"rewarded", progress.rewarded}, {"failed", progress.failed}};
}

QuestProgress ReadProgress(object const& object)
{
    Fields(object, {"counters", "inLog", "readyToReward", "rewarded", "failed"});
    QuestProgress progress;
    auto const& counters = object.at("counters").as_array();
    if (counters.size() != progress.counters.size())
        throw std::invalid_argument("invalid quest checkpoint");
    for (std::size_t index = 0; index < counters.size(); ++index)
        progress.counters[index] = UInt<uint32_t>({{"count", counters[index]}}, "count");
    progress.inLog = object.at("inLog").as_bool();
    progress.readyToReward = object.at("readyToReward").as_bool();
    progress.rewarded = object.at("rewarded").as_bool();
    progress.failed = object.at("failed").as_bool();
    return progress;
}

object EncodeInformation(InformationSearch const& information)
{
    return {{"status", uint8_t(information.status)}, {"attempts", information.attempts},
        {"askedMs", information.askedMs}, {"expiresMs", information.expiresMs},
        {"deliveredMs", information.deliveredMs}, {"leadReport", information.leadReport},
        {"question", information.question}};
}

InformationSearch ReadInformation(object const& object)
{
    Fields(object, {"status", "attempts", "askedMs", "expiresMs", "deliveredMs", "leadReport", "question"});
    return {InformationStatus(UInt<uint8_t>(object, "status")), UInt<uint32_t>(object, "attempts"),
        UInt<uint64_t>(object, "askedMs"), UInt<uint64_t>(object, "expiresMs"),
        UInt<uint64_t>(object, "deliveredMs"), UInt<uint64_t>(object, "leadReport"), String(object, "question", 255)};
}

value EncodeCooperation(Cooperation const& cooperation)
{
    if (cooperation.state == CooperationState::None)
        return nullptr;
    array agreements;
    for (auto const& [actor, agreement] : cooperation.agreements)
        agreements.emplace_back(object{{"actor", Actor(actor)}, {"name", agreement.person.name},
            {"statement", agreement.statement}, {"agreedMs", agreement.agreedMs},
            {"reportedBy", agreement.reportedBy ? value(object{{"actor", MaybeActor(agreement.reportedBy->actor)},
                {"name", agreement.reportedBy->name}}) : value(nullptr)}});
    return object{{"state", uint8_t(cooperation.state)}, {"owner", Actor(cooperation.owner)},
        {"leader", Actor(cooperation.leader)}, {"objective", cooperation.objective}, {"quest", cooperation.quest},
        {"place", cooperation.rendezvousPlace}, {"attempts", cooperation.attempts},
        {"startedMs", cooperation.startedMs}, {"deadlineMs", cooperation.deadlineMs},
        {"reconsiderMs", cooperation.reconsiderMs}, {"reason", cooperation.reason}, {"agreements", agreements}};
}

Cooperation ReadCooperation(value const& value, uint32_t version)
{
    if (value.is_null())
        return {};
    auto const& object = value.as_object();
    Fields(object, {"state", "owner", "leader", "objective", "quest", "place", "attempts", "startedMs",
        "deadlineMs", "reconsiderMs", "reason", "agreements"});
    Cooperation result;
    result.state = CooperationState(UInt<uint8_t>(object, "state"));
    result.owner = ReadActor(object.at("owner"));
    result.leader = ReadActor(object.at("leader"));
    result.objective = UInt<uint64_t>(object, "objective");
    result.quest = UInt<uint32_t>(object, "quest");
    result.rendezvousPlace = UInt<uint32_t>(object, "place");
    result.attempts = UInt<uint32_t>(object, "attempts");
    result.startedMs = UInt<uint64_t>(object, "startedMs");
    result.deadlineMs = UInt<uint64_t>(object, "deadlineMs");
    result.reconsiderMs = UInt<uint64_t>(object, "reconsiderMs");
    result.reason = String(object, "reason", 2048);
    auto const& agreements = object.at("agreements").as_array();
    if (agreements.size() > 5)
        throw std::invalid_argument("too many agreed companions");
    for (auto const& value : agreements)
    {
        auto const& agreement = value.as_object();
        if (version >= 5)
            Fields(agreement, {"actor", "name", "statement", "agreedMs", "reportedBy"});
        else
            Fields(agreement, {"actor", "name", "statement", "agreedMs"});
        auto const actor = ReadActor(agreement.at("actor"));
        CompanionAgreement item{{actor, String(agreement, "name", 400)}, String(agreement, "statement", 2048),
            UInt<uint64_t>(agreement, "agreedMs")};
        if (version >= 5 && !agreement.at("reportedBy").is_null())
        {
            auto const& source = agreement.at("reportedBy").as_object();
            Fields(source, {"actor", "name"});
            item.reportedBy = Reference{ReadMaybeActor(source.at("actor")), String(source, "name", 400)};
        }
        if (!result.agreements.emplace(actor, std::move(item)).second)
            throw std::invalid_argument("duplicate companion agreement");
    }
    if (!IsValidCooperation(result))
        throw std::invalid_argument("invalid cooperation");
    return result;
}

value EncodeRequest(std::optional<HumanRequest> const& request)
{
    if (!request)
        return nullptr;
    return object{{"action", uint8_t(request->action)}, {"source", MaybeActor(request->source.actor)},
        {"name", request->source.name}, {"statement", request->statement}, {"targetName", request->targetName},
        {"acceptedMs", request->acceptedMs}, {"expiresMs", request->expiresMs}};
}

std::optional<HumanRequest> ReadRequest(value const& value)
{
    if (value.is_null())
        return std::nullopt;
    auto const& object = value.as_object();
    Fields(object, {"action", "source", "name", "statement", "targetName", "acceptedMs", "expiresMs"});
    return HumanRequest{RequestAction(UInt<uint8_t>(object, "action")),
        {ReadMaybeActor(object.at("source")), String(object, "name", 400)}, String(object, "statement", 1020),
        String(object, "targetName", 400), UInt<uint64_t>(object, "acceptedMs"), UInt<uint64_t>(object, "expiresMs")};
}

value EncodePreparation(std::optional<ResourcePreparation> const& preparation)
{
    if (!preparation)
        return nullptr;
    return object{{"state", uint8_t(preparation->state)}, {"attempts", preparation->attempts},
        {"transactions", preparation->transactions}, {"startedMs", preparation->startedMs},
        {"deadlineMs", preparation->deadlineMs}, {"reconsiderMs", preparation->reconsiderMs},
        {"lastTransactionMs", preparation->lastTransactionMs}, {"spentMoney", preparation->spentMoney},
        {"reason", preparation->reason}, {"kind", uint8_t(preparation->kind)},
        {"item", preparation->item}, {"count", preparation->count},
        {"attemptsInCircumstances", preparation->attemptsInCircumstances}, {"ownMoney", preparation->ownMoney},
        {"fundsAtAttempt", preparation->fundsAtAttempt}, {"fundsKnown", preparation->fundsKnown},
        {"earnedMoney", preparation->earnedMoney}};
}

std::optional<ResourcePreparation> ReadPreparation(value const& value, uint32_t version)
{
    if (value.is_null())
        return std::nullopt;
    auto const& object = value.as_object();
    if (version == 7)
        Fields(object, {"state", "attempts", "transactions", "startedMs", "deadlineMs", "reconsiderMs",
            "lastTransactionMs", "spentMoney", "reason"});
    else if (version == 8)
        Fields(object, {"state", "attempts", "transactions", "startedMs", "deadlineMs", "reconsiderMs",
            "lastTransactionMs", "spentMoney", "reason", "kind", "item", "count", "attemptsInCircumstances",
            "ownMoney", "fundsAtAttempt", "fundsKnown"});
    else
        Fields(object, {"state", "attempts", "transactions", "startedMs", "deadlineMs", "reconsiderMs",
            "lastTransactionMs", "spentMoney", "reason", "kind", "item", "count", "attemptsInCircumstances",
            "ownMoney", "fundsAtAttempt", "fundsKnown", "earnedMoney"});
    auto result = ResourcePreparation{PreparationState(UInt<uint8_t>(object, "state")),
        UInt<uint32_t>(object, "attempts"), UInt<uint32_t>(object, "transactions"),
        UInt<uint64_t>(object, "startedMs"), UInt<uint64_t>(object, "deadlineMs"),
        UInt<uint64_t>(object, "reconsiderMs"), UInt<uint64_t>(object, "lastTransactionMs"),
        UInt<uint64_t>(object, "spentMoney"), String(object, "reason", 2048)};
    if (version >= 8)
    {
        result.kind = PreparationKind(UInt<uint8_t>(object, "kind"));
        result.item = UInt<uint32_t>(object, "item");
        result.count = UInt<uint32_t>(object, "count");
        result.attemptsInCircumstances = UInt<uint32_t>(object, "attemptsInCircumstances");
        result.ownMoney = UInt<uint32_t>(object, "ownMoney");
        result.fundsAtAttempt = UInt<uint32_t>(object, "fundsAtAttempt");
        result.fundsKnown = object.at("fundsKnown").as_bool();
    }
    else
        result.attemptsInCircumstances = result.attempts;
    if (version >= 9)
        result.earnedMoney = UInt<uint64_t>(object, "earnedMoney");
    return result;
}

object EncodeObjective(Objective const& objective)
{
    array evidence;
    for (auto id : objective.evidence)
        evidence.push_back(id);
    object result{{"id", objective.id}, {"revision", objective.revision}, {"parent", objective.parent},
        {"quest", objective.quest}, {"place", objective.place}, {"person", MaybeActor(objective.person)},
        {"state", uint8_t(objective.state)}, {"step", uint8_t(objective.step)},
        {"obstruction", uint8_t(objective.obstruction)}, {"outcome", objective.outcome}, {"reason", objective.reason},
        {"approach", objective.approach}, {"evidence", std::move(evidence)},
        {"checkpoint", Progress(objective.checkpoint)}, {"lastProgressMs", objective.lastProgressMs},
        {"activeWithoutProgressMs", objective.activeWithoutProgressMs},
        {"nextReconsiderationMs", objective.nextReconsiderationMs}, {"circumstances", objective.circumstances},
        {"attempts", objective.attempts}, {"attemptsInCircumstances", objective.attemptsInCircumstances},
        {"deaths", objective.deaths}, {"gainedCredit", objective.gainedCredit},
        {"arrivedMs", objective.arrivedMs}, {"discoveredQuest", objective.discoveredQuest},
        {"information", EncodeInformation(objective.information)}, {"plannedMs", objective.plannedMs},
        {"cooperation", EncodeCooperation(objective.cooperation)}, {"request", EncodeRequest(objective.request)},
        {"preparation", EncodePreparation(objective.preparation)}};
    if (objective.purpose != PlacePurpose::Work)
        result["activity"] = object{{"purpose", uint8_t(objective.purpose)}, {"observedMs", objective.activityMs},
            {"completedMs", objective.completedMs}, {"creditedMs", objective.creditedActivityMs}};
    if (objective.assessedAttempts || objective.satisfactionReceipt)
        result["assessment"] = object{{"attempts", objective.assessedAttempts},
            {"quest", objective.satisfactionReceipt
                ? value(Progress(*objective.satisfactionReceipt)) : value(nullptr)}};
    return result;
}

Objective ReadObjective(object const& object, uint32_t version)
{
    auto fields = object;
    if (version >= 11)
    {
        fields.erase("activity");
        fields.erase("assessment");
    }
    if (version == 1)
        Fields(fields, {"id", "revision", "parent", "quest", "place", "person", "state", "step", "obstruction",
            "outcome", "reason", "approach", "evidence", "checkpoint", "lastProgressMs", "activeWithoutProgressMs",
            "nextReconsiderationMs", "circumstances", "attempts", "attemptsInCircumstances", "deaths", "gainedCredit",
            "arrivedMs", "discoveredQuest"});
    else if (version == 2)
        Fields(fields, {"id", "revision", "parent", "quest", "place", "person", "state", "step", "obstruction",
            "outcome", "reason", "approach", "evidence", "checkpoint", "lastProgressMs", "activeWithoutProgressMs",
            "nextReconsiderationMs", "circumstances", "attempts", "attemptsInCircumstances", "deaths", "gainedCredit",
            "arrivedMs", "discoveredQuest", "information"});
    else if (version == 3)
        Fields(fields, {"id", "revision", "parent", "quest", "place", "person", "state", "step", "obstruction",
            "outcome", "reason", "approach", "evidence", "checkpoint", "lastProgressMs", "activeWithoutProgressMs",
            "nextReconsiderationMs", "circumstances", "attempts", "attemptsInCircumstances", "deaths", "gainedCredit",
            "arrivedMs", "discoveredQuest", "information", "plannedMs"});
    else if (version <= 5)
        Fields(fields, {"id", "revision", "parent", "quest", "place", "person", "state", "step", "obstruction",
            "outcome", "reason", "approach", "evidence", "checkpoint", "lastProgressMs", "activeWithoutProgressMs",
            "nextReconsiderationMs", "circumstances", "attempts", "attemptsInCircumstances", "deaths", "gainedCredit",
            "arrivedMs", "discoveredQuest", "information", "plannedMs", "cooperation"});
    else if (version == 6)
        Fields(fields, {"id", "revision", "parent", "quest", "place", "person", "state", "step", "obstruction",
            "outcome", "reason", "approach", "evidence", "checkpoint", "lastProgressMs", "activeWithoutProgressMs",
            "nextReconsiderationMs", "circumstances", "attempts", "attemptsInCircumstances", "deaths", "gainedCredit",
            "arrivedMs", "discoveredQuest", "information", "plannedMs", "cooperation", "request"});

    else
        Fields(fields, {"id", "revision", "parent", "quest", "place", "person", "state", "step", "obstruction",
            "outcome", "reason", "approach", "evidence", "checkpoint", "lastProgressMs", "activeWithoutProgressMs",
            "nextReconsiderationMs", "circumstances", "attempts", "attemptsInCircumstances", "deaths", "gainedCredit",
            "arrivedMs", "discoveredQuest", "information", "plannedMs", "cooperation", "request", "preparation"});
    Objective objective;
    objective.id = UInt<uint64_t>(object, "id");
    objective.revision = UInt<uint64_t>(object, "revision");
    objective.parent = UInt<uint64_t>(object, "parent");
    objective.quest = UInt<uint32_t>(object, "quest");
    objective.place = UInt<uint32_t>(object, "place");
    objective.person = ReadMaybeActor(object.at("person"));
    objective.state = ObjectiveState(UInt<uint8_t>(object, "state"));
    objective.step = ObjectiveStep(UInt<uint8_t>(object, "step"));
    objective.obstruction = Obstruction(UInt<uint8_t>(object, "obstruction"));
    objective.outcome = String(object, "outcome", 2048);
    objective.reason = String(object, "reason", 2048);
    objective.approach = String(object, "approach", 256);
    auto const& evidence = object.at("evidence").as_array();
    if (evidence.size() > 16)
        throw std::invalid_argument("too much objective evidence");
    for (auto const& id : evidence)
        objective.evidence.push_back(UInt<uint64_t>({{"id", id}}, "id"));
    objective.checkpoint = ReadProgress(object.at("checkpoint").as_object());
    objective.lastProgressMs = UInt<uint64_t>(object, "lastProgressMs");
    objective.activeWithoutProgressMs = UInt<uint64_t>(object, "activeWithoutProgressMs");
    objective.nextReconsiderationMs = UInt<uint64_t>(object, "nextReconsiderationMs");
    objective.circumstances = UInt<uint64_t>(object, "circumstances");
    objective.attempts = UInt<uint32_t>(object, "attempts");
    objective.attemptsInCircumstances = UInt<uint32_t>(object, "attemptsInCircumstances");
    objective.deaths = UInt<uint32_t>(object, "deaths");
    objective.gainedCredit = UInt<uint32_t>(object, "gainedCredit");
    objective.arrivedMs = UInt<uint64_t>(object, "arrivedMs");
    objective.discoveredQuest = UInt<uint32_t>(object, "discoveredQuest");
    if (version >= 7)
        objective.preparation = ReadPreparation(object.at("preparation"), version);
    if (version >= 6)
        objective.request = ReadRequest(object.at("request"));
    if (version >= 4)
        objective.cooperation = ReadCooperation(object.at("cooperation"), version);
    if (version >= 3)
        objective.plannedMs = UInt<uint64_t>(object, "plannedMs");
    if (version >= 2)
        objective.information = ReadInformation(object.at("information").as_object());
    if (version >= 11 && object.contains("activity"))
    {
        auto const& activity = object.at("activity").as_object();
        Fields(activity, {"purpose", "observedMs", "completedMs", "creditedMs"});
        objective.purpose = PlacePurpose(UInt<uint8_t>(activity, "purpose"));
        objective.activityMs = UInt<uint64_t>(activity, "observedMs");
        objective.completedMs = UInt<uint64_t>(activity, "completedMs");
        objective.creditedActivityMs = UInt<uint64_t>(activity, "creditedMs");
        if (objective.purpose == PlacePurpose::Work)
            throw std::invalid_argument("work cannot carry non-work activity evidence");
    }
    if (version >= 11 && object.contains("assessment"))
    {
        auto const& assessment = object.at("assessment").as_object();
        Fields(assessment, {"attempts", "quest"});
        objective.assessedAttempts = UInt<uint32_t>(assessment, "attempts");
        if (!assessment.at("quest").is_null())
            objective.satisfactionReceipt = ReadProgress(assessment.at("quest").as_object());
    }
    return objective;
}

object EncodePlace(KnownPlace const& place)
{
    object result{{"area", place.area}, {"name", place.name}, {"minimumLevel", place.minimumLevel},
        {"maximumLevel", place.maximumLevel}, {"relativeTo", place.relativeTo}, {"direction", place.direction},
        {"origin", uint8_t(place.origin)}, {"visitedMs", place.visitedMs},
        {"lastUsefulWorkMs", place.lastUsefulWorkMs}, {"investigations", place.investigations}};
    if (place.repair)
        result["repair"] = object{{"map", place.repair->map}, {"phase", place.repair->phase},
            {"x", place.repair->x}, {"y", place.repair->y}, {"z", place.repair->z},
            {"observedMs", place.repair->observedMs}};
    return result;
}

KnownPlace ReadPlace(object const& object, uint32_t version)
{
    if (version >= 10 && object.contains("repair"))
        Fields(object, {"area", "name", "minimumLevel", "maximumLevel", "relativeTo", "direction", "origin",
            "visitedMs", "lastUsefulWorkMs", "investigations", "repair"});
    else
        Fields(object, {"area", "name", "minimumLevel", "maximumLevel", "relativeTo", "direction", "origin",
            "visitedMs", "lastUsefulWorkMs", "investigations"});
    KnownPlace place;
    place.area = UInt<uint32_t>(object, "area");
    place.name = String(object, "name", 400);
    place.minimumLevel = UInt<uint8_t>(object, "minimumLevel");
    place.maximumLevel = UInt<uint8_t>(object, "maximumLevel");
    place.relativeTo = UInt<uint32_t>(object, "relativeTo");
    place.direction = String(object, "direction", 400);
    place.origin = KnowledgeOrigin(UInt<uint8_t>(object, "origin"));
    place.visitedMs = UInt<uint64_t>(object, "visitedMs");
    place.lastUsefulWorkMs = UInt<uint64_t>(object, "lastUsefulWorkMs");
    place.investigations = UInt<uint32_t>(object, "investigations");
    if (version >= 10 && object.contains("repair"))
    {
        auto const& repair = object.at("repair").as_object();
        Fields(repair, {"map", "phase", "x", "y", "z", "observedMs"});
        place.repair = RepairLocation{UInt<uint32_t>(repair, "map"), UInt<uint32_t>(repair, "phase"),
            repair.at("x").to_number<float>(), repair.at("y").to_number<float>(), repair.at("z").to_number<float>(),
            UInt<uint64_t>(repair, "observedMs")};
    }
    return place;
}

object EncodeReport(LearnedReport const& report)
{
    return {{"id", report.id}, {"source", object{{"actor", MaybeActor(report.source.actor)},
        {"name", report.source.name}}}, {"topic", object{{"activity", uint8_t(report.topic.activity)},
        {"quest", report.topic.quest}, {"place", report.topic.place}, {"person", MaybeActor(report.topic.person)}}},
        {"text", report.text}, {"receivedMs", report.receivedMs}, {"confidence", report.confidence},
        {"usefulVisits", report.usefulVisits}, {"unsuccessfulVisits", report.unsuccessfulVisits},
        {"lastAssessedVisitMs", report.lastAssessedVisitMs}};
}

LearnedReport ReadReport(object const& object)
{
    Fields(object, {"id", "source", "topic", "text", "receivedMs", "confidence", "usefulVisits",
        "unsuccessfulVisits", "lastAssessedVisitMs"});
    auto const& source = object.at("source").as_object();
    auto const& topic = object.at("topic").as_object();
    Fields(source, {"actor", "name"});
    Fields(topic, {"activity", "quest", "place", "person"});
    LearnedReport report;
    report.id = UInt<uint64_t>(object, "id");
    report.source = {ReadMaybeActor(source.at("actor")), String(source, "name", 400)};
    report.topic = {Activity(UInt<uint8_t>(topic, "activity")), UInt<uint32_t>(topic, "quest"),
        UInt<uint32_t>(topic, "place"), ReadMaybeActor(topic.at("person"))};
    report.text = String(object, "text", 2048);
    report.receivedMs = UInt<uint64_t>(object, "receivedMs");
    report.confidence = object.at("confidence").to_number<double>();
    report.usefulVisits = UInt<uint32_t>(object, "usefulVisits");
    report.unsuccessfulVisits = UInt<uint32_t>(object, "unsuccessfulVisits");
    report.lastAssessedVisitMs = UInt<uint64_t>(object, "lastAssessedVisitMs");
    return report;
}
}

std::string EncodePlanning(PlanningSnapshot const& snapshot)
{
    if (!IsValidPlanningSnapshot(snapshot))
        throw std::invalid_argument("invalid planning snapshot");
    array objectives, places, reports;
    for (auto const& [id, objective] : snapshot.objectives.objectives)
        objectives.push_back(EncodeObjective(objective));
    for (auto const& [id, place] : snapshot.knowledge.places)
        places.push_back(EncodePlace(place));
    for (auto const& [id, report] : snapshot.knowledge.reports)
        reports.push_back(EncodeReport(report));
    object root{{"version", 15}, {"owner", Actor(snapshot.owner)},
        {"revision", snapshot.revision}, {"nextObjectiveId", snapshot.objectives.nextId},
        {"objectives", std::move(objectives)}, {"seedVersion", snapshot.knowledge.seedVersion},
        {"nextReportId", snapshot.knowledge.nextReport}, {"places", std::move(places)},
        {"reports", std::move(reports)}, {"satisfaction", EncodeSatisfaction(snapshot.satisfaction)}};
    if (!snapshot.knowledge.contacts.empty())
    {
        array contacts;
        for (auto const& [actor, contact] : snapshot.knowledge.contacts)
            contacts.emplace_back(object{{"actor", Actor(actor)}, {"name", contact.person.name},
                {"place", contact.place}, {"map", contact.location.map}, {"phase", contact.location.phase},
                {"x", contact.location.x}, {"y", contact.location.y}, {"z", contact.location.z},
                {"observedMs", contact.location.observedMs}});
        root["contacts"] = std::move(contacts);
    }
    auto encoded = boost::json::serialize(root);
    if (encoded.size() > MaxPlanningBytes)
        throw std::invalid_argument("planning snapshot exceeds bounds");
    return encoded;
}

std::optional<PlanningSnapshot> DecodePlanning(std::string_view text, ActorKey expectedOwner)
{
    try
    {
        // Share the strict duplicate-key/depth parser; the bridge's default 64 KiB frame limit is unchanged.
        auto const decoded = Bridge::Parse(text, MaxPlanningBytes);
        auto const& object = decoded.as_object();
        auto const version = UInt<uint32_t>(object, "version");
        if (version < 1 || version > 15)
            return std::nullopt;
        if (version >= 11 && object.contains("contacts"))
            Fields(object, {"version", "owner", "revision", "nextObjectiveId", "objectives", "seedVersion",
                "nextReportId", "places", "reports", "satisfaction", "contacts"});
        else if (version >= 11)
            Fields(object, {"version", "owner", "revision", "nextObjectiveId", "objectives", "seedVersion",
                "nextReportId", "places", "reports", "satisfaction"});
        else
            Fields(object, {"version", "owner", "revision", "nextObjectiveId", "objectives", "seedVersion",
                "nextReportId", "places", "reports"});
        PlanningSnapshot snapshot;
        if (version >= 11)
            snapshot.satisfaction = ReadSatisfaction(object.at("satisfaction").as_object(), version);
        if (version >= 11 && object.contains("contacts"))
        {
            auto const& contacts = object.at("contacts").as_array();
            if (contacts.size() > 32)
                return std::nullopt;
            for (auto const& value : contacts)
            {
                auto const& contact = value.as_object();
                Fields(contact, {"actor", "name", "place", "map", "phase", "x", "y", "z", "observedMs"});
                auto const actor = ReadActor(contact.at("actor"));
                KnownContact item{{actor, String(contact, "name", 400)}, UInt<uint32_t>(contact, "place"),
                    {UInt<uint32_t>(contact, "map"), UInt<uint32_t>(contact, "phase"),
                        contact.at("x").to_number<float>(), contact.at("y").to_number<float>(),
                        contact.at("z").to_number<float>(), UInt<uint64_t>(contact, "observedMs")}};
                if (!snapshot.knowledge.contacts.emplace(actor, std::move(item)).second)
                    return std::nullopt;
            }
        }
        snapshot.owner = ReadActor(object.at("owner"));
        snapshot.revision = UInt<uint64_t>(object, "revision");
        snapshot.objectives.nextId = UInt<uint64_t>(object, "nextObjectiveId");
        snapshot.knowledge.seedVersion = UInt<uint32_t>(object, "seedVersion");
        snapshot.knowledge.nextReport = UInt<uint64_t>(object, "nextReportId");
        auto const& objectives = object.at("objectives").as_array();
        auto const& places = object.at("places").as_array();
        auto const& reports = object.at("reports").as_array();
        if (snapshot.owner != expectedOwner || objectives.size() > 32 || places.size() > 64 || reports.size() > 64)
            return std::nullopt;
        for (auto const& value : objectives)
        {
            auto objective = ReadObjective(value.as_object(), version);
            if (!snapshot.objectives.objectives.emplace(objective.id, std::move(objective)).second)
                return std::nullopt;
        }
        for (auto const& value : places)
        {
            auto place = ReadPlace(value.as_object(), version);
            if (!snapshot.knowledge.places.emplace(place.area, std::move(place)).second)
                return std::nullopt;
        }
        for (auto const& value : reports)
        {
            auto report = ReadReport(value.as_object());
            if (!snapshot.knowledge.reports.emplace(report.id, std::move(report)).second)
                return std::nullopt;
        }
        if (IsValidPlanningSnapshot(snapshot))
            return snapshot;
    }
    catch (std::exception const&)
    {
        // Corrupt or newer state must never become an empty overwrite of an owner's saved intentions.
    }
    return std::nullopt;
}
}
