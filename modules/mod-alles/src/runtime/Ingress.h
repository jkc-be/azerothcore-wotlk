/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_INGRESS_H
#define MOD_ALLES_INGRESS_H

#include "domain/Memory.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Alles
{
enum class IngressKind
{
    Activate,
    Save,
    Logout,
    Observe
};

struct IngressEvent
{
    IngressKind kind = IngressKind::Observe;
    ActorKey owner;
    uint64_t attachment = 0;
    Perception perception;
};

// One ordered value queue for lifecycle and sensory callbacks. Speech cannot consume the lifecycle reserve.
// No caller may put an ungated observation or gameplay pointer into this queue.
class Ingress
{
public:
    explicit Ingress(std::size_t capacity = 1024, std::size_t lifecycleReserve = 64)
        : _capacity(capacity), _lifecycleReserve(lifecycleReserve)
    {
        if (!capacity || !lifecycleReserve || lifecycleReserve >= capacity)
            throw std::invalid_argument("Invalid alles ingress bounds");
    }

    bool Push(IngressEvent event)
    {
        if (!IsValidActor(event.owner) || (event.kind == IngressKind::Observe && !GatePerception(event.perception)))
            return false;
        std::lock_guard lock(_mutex);
        auto const limit = event.kind == IngressKind::Observe ? _capacity - _lifecycleReserve : _capacity;
        if (_events.size() >= limit)
            return false;
        _events.push_back(std::move(event));
        return true;
    }

    std::vector<IngressEvent> Drain(std::size_t budget)
    {
        std::lock_guard lock(_mutex);
        std::vector<IngressEvent> result;
        result.reserve(std::min(budget, _events.size()));
        while (!_events.empty() && result.size() < budget)
        {
            result.push_back(std::move(_events.front()));
            _events.pop_front();
        }
        return result;
    }

private:
    std::size_t const _capacity;
    std::size_t const _lifecycleReserve;
    std::mutex _mutex;
    std::deque<IngressEvent> _events;
};
}

#endif
