/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "SimulationClock.h"
#include "Timer.h"
#include <atomic>

namespace
{
    std::atomic<bool> enabled{false};
    bool useVirtualTime = true;
    std::atomic<int64> elapsed{0};
    TimePoint steadyOrigin;
    SystemTimePoint systemOrigin;
}

void SimulationClock::Enable(bool virtualTime)
{
    useVirtualTime = virtualTime;
    (void)GetApplicationStartTime();
    steadyOrigin = std::chrono::steady_clock::now();
    systemOrigin = std::chrono::system_clock::now();
    elapsed.store(0);
    enabled.store(true, std::memory_order_release);
}

bool SimulationClock::Enabled()
{
    return enabled.load(std::memory_order_acquire);
}

void SimulationClock::Advance(Milliseconds step)
{
    elapsed.fetch_add(step.count(), std::memory_order_relaxed);
}

Milliseconds SimulationClock::Elapsed()
{
    return Milliseconds(elapsed.load(std::memory_order_relaxed));
}

TimePoint SimulationClock::Now()
{
    return Enabled() && useVirtualTime ? steadyOrigin + Elapsed() : std::chrono::steady_clock::now();
}

SystemTimePoint SimulationClock::SystemNow()
{
    return Enabled() && useVirtualTime ? systemOrigin + Elapsed() : std::chrono::system_clock::now();
}

time_t SimulationClock::Time()
{
    return std::chrono::duration_cast<Seconds>(SystemNow().time_since_epoch()).count();
}
