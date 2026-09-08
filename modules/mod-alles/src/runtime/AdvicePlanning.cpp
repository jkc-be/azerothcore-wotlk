/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "AdvicePlanning.h"
#include "ConversationPolicy.h"
#include <boost/json.hpp>
#include <algorithm>

namespace Alles
{
namespace
{
bool CurrentReply(InformationReply const& reply, Objective const* objective,
    PrivateKnowledge const& knowledge, uint64_t now)
{
    return objective && objective->state != ObjectiveState::Completed && objective->state != ObjectiveState::Cancelled
        && objective->information.status == InformationStatus::Awaiting
        && objective->information.attempts == reply.question.attempt
        && objective->information.question == reply.question.text && now < objective->information.expiresMs
        && reply.gameMs >= objective->information.deliveredMs && reply.gameMs <= now
        && reply.gameMs < objective->information.expiresMs
        && ((reply.question.topic.activity == Activity::Work && reply.question.topic.quest == objective->quest
                && reply.question.topic.place == objective->place && !reply.question.topic.person)
            || MatchesRecruitment(reply.question, *objective, knowledge, now));
}
}

bool MatchesRecruitment(InformationQuestion const& question, Objective const& objective,
    PrivateKnowledge const& knowledge, uint64_t now)
{
    auto const& cooperation = objective.cooperation;
    auto const place = knowledge.Places().find(question.topic.place);
    auto const self = cooperation.agreements.find(question.owner);
    return question.topic.activity == Activity::Companions && !question.topic.person
        && question.objective == objective.id && question.topic.quest == objective.quest
        && question.owner == cooperation.owner && question.owner == cooperation.leader
        && question.topic.place == cooperation.rendezvousPlace
        && (cooperation.state == CooperationState::Recruiting || cooperation.state == CooperationState::Agreed
            || cooperation.state == CooperationState::Rendezvous)
        && question.partySize >= 2 && question.partySize <= 5
        && now >= cooperation.startedMs && now < cooperation.deadlineMs
        && !question.questName.empty() && IsBoundedText(question.questName, 100)
        && place != knowledge.Places().end() && question.placeName == place->second.name
        && question.text.find(question.questName) != std::string::npos
        && question.text.find(question.placeName) != std::string::npos
        && self != cooperation.agreements.end() && self->second.statement == question.text;
}

CapabilityRegistry RecruitmentCapabilities()
{
    CapabilityRegistry registry;
    registry.Register({"none", false, false, false, "No clear willingness to join the requested quest",
        "Keep recruiting within the existing deadline", "No ongoing action"});
    registry.Register({"invite_companion", true, false, true, "The actual speaker explicitly offers to join this quest",
        "Record the speaker's offer and attempt a normal invitation; membership is observed separately",
        "Expire the agreement on timeout or yield to human control"});
    return registry;
}

bool CanOfferHelp(ActorKey owner, ObjectiveBook const& book, RecruitmentNotice const& notice,
    QuestProgress const& progress, bool canAccept, uint64_t now)
{
    if (book.Following() || book.Preparing())
        return false;
    auto const& question = notice.question;
    if (!IsValidActor(owner) || owner.kind != ActorKind::Player || owner == question.owner
        || notice.source.actor != question.owner || question.owner.kind != ActorKind::Player
        || !IsValidActor(question.owner) || !question.generation || !question.objective || !question.attempt
        || question.partySize < 2 || question.partySize > 5
        || question.topic.activity != Activity::Companions || !question.topic.quest || !question.topic.place
        || question.topic.person || question.questName.empty() || !IsBoundedText(question.questName, 100)
        || question.placeName.empty() || !IsBoundedText(question.placeName, 100)
        || question.text.find(question.questName) == std::string::npos
        || question.text.find(question.placeName) == std::string::npos
        || now < notice.receivedMs || now - notice.receivedMs >= 120000
        || progress.failed || progress.readyToReward || (!progress.inLog && !progress.rewarded && !canAccept))
        return false;
    if (auto const* current = book.Current(); current && current->quest && current->quest != question.topic.quest)
        return false;
    for (auto const& [id, objective] : book.All())
    {
        if (objective.quest != question.topic.quest && objective.checkpoint.readyToReward
            && objective.state != ObjectiveState::Completed && objective.state != ObjectiveState::Cancelled)
            return false;
        auto const& cooperation = objective.cooperation;
        if (cooperation.state > CooperationState::None && cooperation.state < CooperationState::Completed)
            return false;
        if (objective.quest == question.topic.quest
            && ((objective.state == ObjectiveState::Completed && !progress.rewarded)
            || objective.state == ObjectiveState::Cancelled
            || (cooperation.state != CooperationState::None && (cooperation.state != CooperationState::Deferred
                || cooperation.attempts >= 2 || now < cooperation.reconsiderMs))))
            return false;
    }
    return true;
}

std::optional<uint64_t> RecordHelpOffer(ActorKey owner, std::string const& name, ObjectiveBook& book,
    PrivateKnowledge& knowledge, RecruitmentNotice const& notice, QuestProgress const& progress,
    bool canAccept, std::string const& statement, uint64_t now)
{
    if (!CanOfferHelp(owner, book, notice, progress, canAccept, now))
        return std::nullopt;
    auto stagedBook = book;
    auto stagedKnowledge = knowledge;
    auto const& question = notice.question;
    if (!stagedKnowledge.LearnPlace(question.topic.place, question.placeName)
        || !stagedKnowledge.Hear(notice.source, question.topic, question.text, notice.receivedMs, 0.4))
        return std::nullopt;
    Objective const* objective = nullptr;
    for (auto const& [id, retained] : stagedBook.All())
        if (retained.quest == question.topic.quest)
            objective = &retained;
    if (!objective)
        objective = stagedBook.ProposeQuest(question.topic.quest,
            (progress.rewarded ? "Help companions with " : "Earn the reward for ") + question.questName,
            "A delivered invitation to cooperate", progress);
    if (!objective)
        return std::nullopt;
    auto const id = objective->id;
    auto cooperation = objective->cooperation;
    if (!AcceptCooperation(cooperation, owner, id, question.topic.quest, question.topic.place,
        {{owner, name}, statement, now}, {notice.source, question.text, notice.receivedMs}, now)
        || !stagedBook.SetCooperation(id, objective->revision, std::move(cooperation)))
        return std::nullopt;
    if (auto const* current = stagedBook.Current(); current && current->id != id)
    {
        if (!stagedBook.Block(current->id, Obstruction::Executor, "Honor my offer to join the agreed quest", now))
            return std::nullopt;
        stagedBook.Defer(current->id, now);
    }
    if (progress.rewarded && objective->state != ObjectiveState::Completed
        && !stagedBook.Observe(id, progress, ObjectiveStep::Wait, now))
        return std::nullopt;
    if (!stagedBook.AwaitCooperation(id, now))
        return std::nullopt;
    book = std::move(stagedBook);
    knowledge = std::move(stagedKnowledge);
    return id;
}

CapabilityRegistry AdviceCapabilities()
{
    CapabilityRegistry registry;
    registry.Register({"none", false, false, false, "No useful answer in the delivered line",
        "Keep current knowledge and gameplay", "No ongoing action"});
    registry.Register({"remember_report", false, false, false, "Relevant uncertain advice or a warning",
        "Retain the speaker and exact statement; no changed intention", "No ongoing action"});
    registry.Register({"remember_place_report", false, true, false, "Relevant report naming a supplied heard place",
        "Retain uncertain geography and exact source statement; no changed intention", "No ongoing action"});
    registry.Register({"investigate_report", false, true, false, "Positive actionable lead at a supplied heard place",
        "Queue ordinary investigation; only observed new work can complete it", "Release investigation on handoff"});
    registry.Register({"retry_quest", true, false, false, "Concrete advice resolving the accepted quest's obstruction",
        "Reconsider the quest; only observed progress or reward establishes success", "Release quest on handoff"});
    return registry;
}

std::map<uint32_t, std::string> HeardPlaces(std::string const& text,
    std::map<uint32_t, std::string> const& dictionary, PrivateKnowledge const& knowledge)
{
    auto const message = " " + ConversationFingerprint(text) + " ";
    std::map<std::string, std::map<uint32_t, std::string>> matches;
    for (auto const& [id, name] : dictionary)
    {
        auto const normalized = ConversationFingerprint(name);
        if (id && !normalized.empty() && IsBoundedText(name, 100)
            && message.find(" " + normalized + " ") != std::string::npos)
            matches[normalized].emplace(id, name);
    }
    std::map<uint32_t, std::string> result;
    for (auto const& [name, candidates] : matches)
        for (auto const& [id, label] : candidates)
            if (result.size() < 8 && (candidates.size() == 1 || knowledge.Places().contains(id)))
                result.emplace(id, label);
    return result;
}

std::optional<AdviceJob> PrepareAdvice(ActorKey owner, uint64_t generation, ObjectiveBook const& book,
    PrivateKnowledge const& knowledge, InformationReply reply,
    std::map<uint32_t, std::string> const& dictionary, uint64_t now)
{
    auto const* objective = book.Find(reply.question.objective);
    if (!IsValidActor(owner) || !generation || owner != reply.question.owner || generation != reply.question.generation
        || !CurrentReply(reply, objective, knowledge, now) || reply.text.empty() || !IsBoundedText(reply.text, 512)
        || !IsBoundedText(reply.source.name, 100) || (!reply.source.actor && reply.source.name.empty())
        || (reply.source.actor && (!IsValidActor(*reply.source.actor) || *reply.source.actor == owner)))
        return std::nullopt;
    AdviceJob job;
    job.issued = {owner, generation, objective->id, objective->revision, true, {}, {}, {}};
    bool const recruiting = reply.question.topic.activity == Activity::Companions;
    if (recruiting && (!reply.source.actor || reply.source.actor->kind != ActorKind::Player
        || reply.source.name.empty() || objective->cooperation.agreements.contains(*reply.source.actor)
        || objective->cooperation.agreements.size() >= reply.question.partySize))
        return std::nullopt;
    if (!recruiting)
        job.heardPlaces = HeardPlaces(reply.text, dictionary, knowledge);
    boost::json::array places, quests, people, evidence, capabilities;
    if (recruiting)
    {
        job.issued.people.insert(*reply.source.actor);
        people.emplace_back(boost::json::object{{"kind", uint8_t(reply.source.actor->kind)},
            {"id", reply.source.actor->id}});
    }
    for (auto const& [id, name] : job.heardPlaces)
    {
        job.issued.places.insert(id);
        places.emplace_back(boost::json::object{{"id", id}, {"name", name}, {"basis", "name in delivered reply"}});
    }
    if (objective->quest)
    {
        job.issued.quests.insert(objective->quest);
        quests.emplace_back(boost::json::object{{"id", objective->quest}, {"outcome", objective->outcome}});
    }
    auto registry = recruiting ? RecruitmentCapabilities() : AdviceCapabilities();
    for (auto const& [name, spec] : registry.All())
        if ((!spec.quest || !job.issued.quests.empty()) && (!spec.place || !job.issued.places.empty()))
            capabilities.emplace_back(boost::json::object{{"name", name}, {"quest", spec.quest},
                {"place", spec.place}, {"person", spec.person}, {"precondition", spec.precondition},
                {"observableEffect", spec.observableEffect}, {"cancellation", spec.cancellation}});
    evidence.emplace_back(boost::json::object{{"token", "reply"}, {"source", reply.source.name},
        {"text", reply.text}, {"receivedMs", reply.gameMs}, {"channel", reply.channel}});
    job.context = {{"purpose", recruiting ? "recruitment" : "advice"}, {"gameTimeMs", now},
        {"owner", boost::json::object{{"kind", uint8_t(owner.kind)}, {"id", owner.id}}},
        {"objective", boost::json::object{{"id", objective->id}, {"revision", objective->revision},
            {"outcome", objective->outcome}, {"reason", objective->reason},
            {"obstruction", Name(objective->obstruction)}}},
        {"question", boost::json::object{{"attempt", reply.question.attempt}, {"text", reply.question.text}}},
        {"capabilities", std::move(capabilities)}, {"places", std::move(places)}, {"quests", std::move(quests)},
        {"people", std::move(people)}, {"evidence", std::move(evidence)}};
    if (boost::json::serialize(job.context).size() > 8192)
        return std::nullopt;
    job.reply = std::move(reply);
    return job;
}

AdviceOutcome ApplyAdvice(AdviceJob const& job, Bridge::PlanningDecision const& decision,
    CapabilityContext const& live, ObjectiveBook& book, PrivateKnowledge& knowledge, uint64_t now)
{
    bool const recruiting = job.reply.question.topic.activity == Activity::Companions;
    auto registry = recruiting ? RecruitmentCapabilities() : AdviceCapabilities();
    if (auto const invalid = registry.Validate(decision.request, job.issued); !invalid.empty())
        return {invalid};
    if (auto const invalid = registry.Validate(decision.request, live); !invalid.empty())
        return {invalid};
    auto const* current = book.Find(job.issued.objective);
    if (!current || current->revision != job.issued.revision || !CurrentReply(job.reply, current, knowledge, now))
        return {"stale_information_search"};
    auto const& request = decision.request;
    if (request.capability == "none")
        return {decision.evidence.empty() ? "irrelevant" : "invalid_evidence"};
    if (decision.evidence != "reply" || (request.place && !job.heardPlaces.contains(request.place)))
        return {"invalid_evidence"};
    if (recruiting)
    {
        if (request.person != job.reply.source.actor || request.quest != current->quest)
            return {"speaker_or_quest_mismatch"};
        auto cooperation = current->cooperation;
        if (!AgreeCompanion(cooperation, {job.reply.source, job.reply.text, job.reply.gameMs}, now)
            || !book.SetCooperation(current->id, current->revision, std::move(cooperation)))
            return {"agreement_not_applicable"};
        return {"companion_agreed", 0, current->id, request.person};
    }
    auto stagedBook = book;
    auto stagedKnowledge = knowledge;
    auto topic = job.reply.question.topic;
    topic.place = request.place;
    if (request.place && !stagedKnowledge.LearnPlace(request.place, job.heardPlaces.at(request.place)))
        return {"knowledge_capacity"};
    auto const report = stagedKnowledge.Hear(job.reply.source, topic, job.reply.text, job.reply.gameMs, 0.4);
    if (!report)
        return {"invalid_report"};
    uint64_t intention = 0;
    if (request.capability == "investigate_report")
    {
        auto const child = stagedBook.InvestigateLead(request.objective, job.reply.question.attempt, *report,
            request.place, job.heardPlaces.at(request.place), now);
        if (!child)
            return {"lead_not_applicable"};
        intention = *child;
    }
    else if (request.capability == "retry_quest")
    {
        if (!stagedBook.InformationLead(request.objective, job.reply.question.attempt, *report, now))
            return {"lead_not_applicable"};
        intention = request.objective;
    }
    book = std::move(stagedBook);
    knowledge = std::move(stagedKnowledge);
    return {intention ? "lead_applied" : "report_retained", *report, intention};
}
}
