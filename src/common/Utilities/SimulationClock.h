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

#ifndef AC_SIMULATION_CLOCK_H
#define AC_SIMULATION_CLOCK_H

#include "Define.h"
#include "Duration.h"
#include <ctime>

// Opt-in, process-wide gameplay time. Only the world thread advances it, between joined map updates.
namespace SimulationClock
{
    AC_COMMON_API void Enable(bool virtualTime = true);
    AC_COMMON_API bool Enabled();
    AC_COMMON_API void Advance(Milliseconds step);
    AC_COMMON_API Milliseconds Elapsed();
    AC_COMMON_API TimePoint Now();
    AC_COMMON_API SystemTimePoint SystemNow();
    AC_COMMON_API time_t Time();
} // namespace SimulationClock

#endif
