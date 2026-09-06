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

#ifndef AC_SIMULATION_BUDGET_H
#define AC_SIMULATION_BUDGET_H

#include "Define.h"

class SimulationBudget
{
public:
    static constexpr uint32 StepMs = 10;

    void Accrue(uint64 realMicroseconds, uint32 speed, bool paused)
    {
        if (!paused)
            _debt += realMicroseconds * speed;
    }

    bool Consume(bool paused)
    {
        if (paused || _debt < StepMs * 1000)
            return false;
        _debt -= StepMs * 1000;
        return true;
    }

    uint64 DebtMicroseconds() const { return _debt; }

private:
    uint64 _debt = 0;
};

#endif
