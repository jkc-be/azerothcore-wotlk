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

#include "SimulationBudget.h"
#include <gtest/gtest.h>

TEST(SimulationBudget, AccelerationChangesNumberOfNormalSteps)
{
    for (uint32 speed : {1, 2, 5, 10})
    {
        SimulationBudget budget;
        budget.Accrue(1000000, speed, false);
        uint32 ticks = 0;
        while (budget.Consume(false))
            ++ticks;
        EXPECT_EQ(ticks * SimulationBudget::StepMs, speed * 1000);
        EXPECT_EQ(budget.DebtMicroseconds(), 0);
    }
}

TEST(SimulationBudget, PausePreservesDebtWithoutAccumulatingPausedWallTime)
{
    SimulationBudget budget;
    budget.Accrue(10000, 10, false);
    EXPECT_TRUE(budget.Consume(false));
    budget.Accrue(1000000, 10, true);
    EXPECT_FALSE(budget.Consume(true));
    EXPECT_EQ(budget.DebtMicroseconds(), 90000);
    EXPECT_TRUE(budget.Consume(false));
    EXPECT_EQ(budget.DebtMicroseconds(), 80000);
}

TEST(SimulationBudget, SpeedChangesAndOverloadNeverDiscardFractionalOrOutstandingSteps)
{
    SimulationBudget budget;
    budget.Accrue(900, 1, false);
    EXPECT_FALSE(budget.Consume(false));
    budget.Accrue(1000000, 10, false);
    budget.Accrue(50, 2, false);
    uint32 ticks = 0;
    while (budget.Consume(false))
        ++ticks;
    EXPECT_EQ(ticks, 1000);
    EXPECT_EQ(budget.DebtMicroseconds(), 1000);
}
