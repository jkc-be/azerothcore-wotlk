/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_RUNTIME_SETTINGS_H
#define MOD_ALLES_RUNTIME_SETTINGS_H

#include "domain/ActorStore.h"
#include "bridge/Service.h"
#include <set>

namespace Alles
{
struct RuntimeSettings
{
    std::set<ActorKey> owners;
    StoreLimits limits;
    MemoryPolicy memory;
    std::size_t ingressCapacity = 1024;
    std::size_t itemBudget = 32;
    uint64_t shutdownBudgetMs = 10000;
    uint64_t speechCooldownMs = 30000;
    uint64_t speechRepeatMs = 300000;
    float witnessRange = 40;
    bool speech = true;
    bool withholdFake = false;
    bool external = false;
    bool conversation = false;
    Bridge::Settings bridge;
    std::string telemetryDirectory;
    uint64_t telemetrySegmentBytes = 64 * 1024 * 1024; // 0 keeps one growing journal per stream
};

// Exact typed decimal entries: player:ID or creature:ID, separated by commas. No inferred GUID kind.
std::optional<std::set<ActorKey>> ParseOwners(std::string_view text, std::size_t cap);
RuntimeSettings ReadRuntimeSettings();
} // namespace Alles

#endif
