/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_CAPABILITY_H
#define MOD_ALLES_CAPABILITY_H

#include "Memory.h"
#include <map>
#include <set>

namespace Alles
{
// Registry entries describe installed execution adapters. A worker may only use entries supplied in its job.
struct CapabilitySpec
{
    std::string name;
    bool quest = false;
    bool place = false;
    bool person = false;
    std::string precondition;
    std::string observableEffect;
    std::string cancellation;
};

struct CapabilityRequest
{
    uint32_t contractVersion = 1;
    ActorKey owner;
    uint64_t actorGeneration = 0;
    uint64_t objective = 0;
    uint64_t revision = 0;
    std::string capability;
    uint32_t quest = 0;
    uint32_t place = 0;
    std::optional<ActorKey> person;
};

struct CapabilityContext
{
    ActorKey owner;
    uint64_t actorGeneration = 0;
    uint64_t objective = 0;
    uint64_t revision = 0;
    bool autonomous = false;
    std::set<uint32_t> quests;
    std::set<uint32_t> places;
    std::set<ActorKey> people;
};

class CapabilityRegistry
{
public:
    bool Register(CapabilitySpec spec);
    // Empty means the supplied references and ownership fence are valid. Adapters still check live preconditions.
    std::string Validate(CapabilityRequest const& request, CapabilityContext const& context) const;
    std::map<std::string, CapabilitySpec> const& All() const { return _specs; }

private:
    std::map<std::string, CapabilitySpec> _specs;
};
}
#endif
