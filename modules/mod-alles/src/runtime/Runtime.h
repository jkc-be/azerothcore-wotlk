/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_RUNTIME_H
#define MOD_ALLES_RUNTIME_H

#include "Ingress.h"
#include "RuntimeSettings.h"
#include <atomic>
#include <memory>
#include <thread>

class Player;
class Unit;
class WorldPacket;

namespace Alles
{
// Published once after construction. Hook threads only capture/enqueue values; the main thread owns the store.
class Runtime
{
public:
    explicit Runtime(RuntimeSettings settings);
    ~Runtime();
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;

    void Update();
    void BeginShutdown();
    void FinishShutdown();
    void Login(Player& player);
    void Lifecycle(Player& player, IngressKind kind);
    void Packet(Player& receiver, WorldPacket const& packet);
    void OwnDeath(Player& player);
    void WitnessDeath(Unit& victim, Unit* killer);

    std::vector<std::string> Recall(Player const& player, std::size_t count) const;
    std::optional<OwnerStatus> Status(ActorKey owner) const;
    std::optional<uint64_t> Flush(ActorKey owner);
    bool IsMainThread() const;
    bool Contains(ActorKey owner) const;
    uint64_t Dropped() const;
    uint64_t UnsafePackets() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

Runtime* ActiveRuntime();
void PublishRuntime(Runtime* runtime);
}

#endif
