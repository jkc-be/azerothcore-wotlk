/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_ADVICE_PLANNING_H
#define MOD_ALLES_ADVICE_PLANNING_H

#include "InformationQuestion.h"
#include "bridge/PlanningWire.h"
#include "domain/Objective.h"
#include <boost/json/object.hpp>

namespace Alles
{
struct AdviceJob
{
    CapabilityContext issued;
    InformationReply reply;
    std::map<uint32_t, std::string> heardPlaces;
    boost::json::object context;
};

struct AdviceOutcome
{
    std::string status;
    uint64_t report = 0;
    uint64_t intention = 0;
    std::optional<ActorKey> invite;
};

CapabilityRegistry AdviceCapabilities();
CapabilityRegistry RecruitmentCapabilities();
// Public declarations must name the quest and meeting place from this owner's retained recruitment.
bool MatchesRecruitment(InformationQuestion const& question, Objective const& objective,
    PrivateKnowledge const& knowledge, uint64_t now);
bool CanOfferHelp(ActorKey owner, ObjectiveBook const& book, RecruitmentNotice const& notice,
    QuestProgress const& progress, bool canAccept, uint64_t now);
// Records this owner's explicit offer atomically; no invitation, acceptance, peer progress or movement is inferred.
std::optional<uint64_t> RecordHelpOffer(ActorKey owner, std::string const& name, ObjectiveBook& book,
    PrivateKnowledge& knowledge, RecruitmentNotice const& notice, QuestProgress const& progress,
    bool canAccept, std::string const& statement, uint64_t now);
// The dictionary resolves only whole names present in the delivered text. Ambiguous unknown names are omitted.
std::map<uint32_t, std::string> HeardPlaces(std::string const& text,
    std::map<uint32_t, std::string> const& dictionary, PrivateKnowledge const& knowledge);
std::optional<AdviceJob> PrepareAdvice(ActorKey owner, uint64_t generation, ObjectiveBook const& book,
    PrivateKnowledge const& knowledge, InformationReply reply,
    std::map<uint32_t, std::string> const& dictionary, uint64_t now);
// Applies atomically to this owner's values after current actor/objective/reference and question fences.
AdviceOutcome ApplyAdvice(AdviceJob const& job, Bridge::PlanningDecision const& decision,
    CapabilityContext const& live, ObjectiveBook& book, PrivateKnowledge& knowledge, uint64_t now);
}
#endif
