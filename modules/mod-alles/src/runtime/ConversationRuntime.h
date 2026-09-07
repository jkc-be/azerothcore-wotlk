/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_CONVERSATION_RUNTIME_H
#define MOD_ALLES_CONVERSATION_RUNTIME_H

#include "bridge/Service.h"
#include <memory>

class Player;

namespace Alles
{
class ConversationRuntime
{
public:
    ConversationRuntime(ActorStore& store, Bridge::Service& bridge);
    ~ConversationRuntime();
    void Login(Player& player);
    void Logout(Player& player);
    void Heard(Player& bot, Player& human, Perception const& perception, uint8_t channel);
    void Update(uint64_t gameMs, uint64_t realMs);
    void Stop();
    boost::json::object Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Alles
#endif
