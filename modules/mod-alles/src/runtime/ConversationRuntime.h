/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_CONVERSATION_RUNTIME_H
#define MOD_ALLES_CONVERSATION_RUNTIME_H

#include "bridge/Service.h"
#include "perception/SpeechRoute.h"
#include "InformationQuestion.h"
#include <memory>
#include <vector>

class Player;

namespace Alles
{
class ObjectiveRuntime;
namespace Telemetry
{
class Recorder;
}

class ConversationRuntime
{
public:
    ConversationRuntime(ActorStore& store, Bridge::Service& bridge, Telemetry::Recorder* recorder = nullptr);
    ~ConversationRuntime();
    void Login(Player& player);
    void Logout(Player& player);
    void Heard(Player& bot, Player& human, Perception const& perception, SpeechRoute const& route);
    bool CanAsk(ActorKey owner) const;
    bool Ask(InformationQuestion const& question, uint64_t gameMs, uint64_t realMs);
    bool RecruitmentActive(RecruitmentNotice const& notice, uint64_t realMs) const;
    void SetObjectives(ObjectiveRuntime* objectives);
    bool SendRoster(ActorKey owner, ActorKey recipient, CooperativeRoster const& roster,
        uint64_t gameMs, uint64_t realMs);
    std::vector<InformationReply> TakeInformationReplies(ActorKey owner);
    void Update(uint64_t gameMs, uint64_t realMs);
    void Stop();
    boost::json::object Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Alles
#endif
