/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_OBJECTIVE_RUNTIME_H
#define MOD_ALLES_OBJECTIVE_RUNTIME_H

#include "domain/ActorStore.h"
#include "InformationQuestion.h"
#include <boost/json/object.hpp>
#include <memory>
#include <set>

class Player;
class Unit;
class ObjectGuid;
class WorldPacket;

namespace Alles
{
namespace Telemetry { class Recorder; }
namespace Bridge { class Service; }
class ConversationRuntime;
Unit* HumanRequestThreat(Player& bot, Player& human);

class ObjectiveRuntime
{
public:
    ObjectiveRuntime(ActorStore& store, std::set<ActorKey> owners, Telemetry::Recorder* recorder,
        ConversationRuntime* conversation = nullptr, Bridge::Service* bridge = nullptr, bool autonomousPlanning = true);
    ~ObjectiveRuntime();
    void Update(uint64_t gameMs, uint64_t realMs);
    void Detach(ActorKey owner, uint64_t gameMs, uint64_t realMs);
    void Stop(uint64_t gameMs, uint64_t realMs);
    boost::json::object Status(ActorKey owner) const;
    boost::json::object HelpOfferContext(ActorKey owner, uint64_t generation, RecruitmentNotice const& notice,
        uint64_t gameMs, uint64_t realMs) const;
    bool OfferHelp(ActorKey owner, uint64_t generation, RecruitmentNotice const& notice,
        std::string const& statement, uint64_t gameMs, uint64_t realMs);
    bool ReceiveRoster(ActorKey owner, uint64_t generation, CooperativeRoster const& roster,
        Reference const& source, uint64_t gameMs, uint64_t realMs);
    boost::json::object HumanContext(ActorKey owner, uint64_t generation, ActorKey person) const;
    std::string ApplyHumanRequest(ActorKey owner, uint64_t generation, Reference const& source,
        std::string const& statement, std::string const& action, ObjectGuid const& threat,
        uint64_t gameMs, uint64_t realMs);
    void RequestPacket(Player& receiver, WorldPacket const& packet);
    void RequesterLeft(ActorKey person, uint64_t realMs);
    std::size_t FollowingCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
#endif
