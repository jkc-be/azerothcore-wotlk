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

#ifndef AC_OBSERVATORY_H
#define AC_OBSERVATORY_H

#include "Define.h"
#include <string>
#include <string_view>

class Player;
class Unit;
namespace Observatory
{
    // Called after all configs load, before databases and gameplay objects are initialized.
    AC_GAME_API bool Initialize();
    AC_GAME_API void Stop();
    AC_GAME_API void Fail(std::string_view reason);
    AC_GAME_API bool AllowsBot(uint32 characterId);
    // World-thread-only population contract with the Playerbots module.
    AC_GAME_API uint32 TargetBotCount();
    AC_GAME_API void PopulationSettled(bool settled);
    // World-thread pump; controls and observation keep running while gameplay is paused.
    AC_GAME_API void Run();
    AC_GAME_API void Event(Player const* player, std::string_view kind, uint64 value = 0, std::string_view detail = {});
    AC_GAME_API void Probe(Unit const* actor, std::string_view kind, uint64 value = 0, uint32 spell = 0,
                           Unit const* other = nullptr);
    // Annotates every event produced by a bot action or factory convenience on this thread.
    class AC_GAME_API Context
    {
    public:
        explicit Context(std::string_view name);
        ~Context();

    private:
        std::string _previous;
    };
} // namespace Observatory

#endif
