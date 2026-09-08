/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_COOPERATION_H
#define MOD_ALLES_COOPERATION_H

#include "Memory.h"
#include <map>
#include <vector>

namespace Alles
{
enum class CooperationState : uint8_t
{
    None, Recruiting, Agreed, Rendezvous, Working, Completed, Deferred, Cancelled
};

struct CompanionAgreement
{
    Reference person;
    std::string statement;
    uint64_t agreedMs = 0;
    std::optional<Reference> reportedBy; // A leader's delivered roster report is not the companion's direct statement.

    bool operator==(CompanionAgreement const&) const = default;
};

// A private owner's semantic agreement, not a saved Group pointer, invitation or claim about peer quest credit.
struct Cooperation
{
    CooperationState state = CooperationState::None;
    ActorKey owner;
    ActorKey leader;
    uint64_t objective = 0;
    uint32_t quest = 0;
    uint32_t rendezvousPlace = 0;
    uint32_t attempts = 0;
    uint64_t startedMs = 0;
    uint64_t deadlineMs = 0;
    uint64_t reconsiderMs = 0;
    std::string reason;
    std::map<ActorKey, CompanionAgreement> agreements;

    bool operator==(Cooperation const&) const = default;
};

struct CompanionObservation
{
    ActorKey actor;
    bool online = false;
    bool alive = false;
    bool inRendezvous = false;
    bool eligible = false;
    bool ready = false;
    bool finished = false;
    bool readyToReward = false;
};

struct PartyObservation
{
    uint64_t group = 0;
    ActorKey leader;
    bool ordinaryParty = false;
    bool ownerControlled = false;
    std::vector<CompanionObservation> members;
    uint8_t requiredMembers = 2;
};

bool IsValidCooperation(Cooperation const& state);
bool BeginRecruitment(Cooperation& state, ActorKey owner, uint64_t objective, uint32_t quest,
    uint32_t place, CompanionAgreement self, uint64_t now);
bool AcceptCooperation(Cooperation& state, ActorKey owner, uint64_t objective, uint32_t quest,
    uint32_t place, CompanionAgreement self, CompanionAgreement leader, uint64_t now);
bool AgreeCompanion(Cooperation& state, CompanionAgreement agreement, uint64_t now);
bool AdoptCooperativeRoster(Cooperation& state, Reference const& source, std::vector<Reference> const& members,
    std::string const& statement, uint64_t now);
bool ObserveCooperation(Cooperation& state, PartyObservation const& observed, uint64_t now);
bool DeferCooperation(Cooperation& state, std::string reason, uint64_t now);
// Prefer the leader while it still needs credit, then another unfinished participant, then outstanding turn-ins.
std::optional<ActorKey> CooperativeGuide(Cooperation const& state, PartyObservation const& observed);
void ReconcileCooperation(Cooperation& state, uint64_t now);
char const* Name(CooperationState state);
}
#endif
