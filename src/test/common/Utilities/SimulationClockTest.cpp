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
#include "EventMap.h"
#include "GameTime.h"
#include "TaskScheduler.h"
#include "Timer.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <thread>

// A subprocess prevents the process-wide simulation clock from affecting unrelated tests.
TEST(SimulationClockDeathTest, GameplayClocksPauseAndAdvanceTogether)
{
    EXPECT_EXIT(
        {
            SimulationClock::Enable();
            GameTime::UpdateGameTimers();
            auto steady = SimulationClock::Now();
            auto system = SimulationClock::SystemNow();
            auto milliseconds = GetTimeMS();
            uint32 oldMilliseconds = getMSTime();
            uint32 real = getRealMSTime();
            bool fired = false;
            TaskScheduler scheduler;
            scheduler.Schedule(20ms, [&](TaskContext) { fired = true; });
            EventMap events;
            events.ScheduleEvent(1, 20ms);
            std::this_thread::sleep_for(30ms);
            scheduler.Update();
            if (fired || SimulationClock::Now() != steady || SimulationClock::SystemNow() != system ||
                GetTimeMS() != milliseconds || getRealMSTime() == real)
                std::exit(1);
            SimulationClock::Advance(10ms);
            GameTime::UpdateGameTimers();
            scheduler.Update();
            events.Update(10);
            if (fired || events.ExecuteEvent() || SimulationClock::Now() != steady + 10ms ||
                SimulationClock::SystemNow() != system + 10ms || GetTimeMS() != milliseconds + 10ms ||
                getMSTime() != oldMilliseconds + 10 || GameTime::Now() != SimulationClock::Now() ||
                GameTime::GetSystemTime() != SimulationClock::SystemNow() || GameTime::GetGameTimeMS() != GetTimeMS())
                std::exit(2);
            SimulationClock::Advance(10ms);
            scheduler.Update();
            events.Update(10);
            std::exit(fired && events.ExecuteEvent() == 1 ? 0 : 3);
        },
        ::testing::ExitedWithCode(0), "");
}
