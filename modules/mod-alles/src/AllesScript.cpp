/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "AllesDatabaseGuard.h"
#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include "runtime/Runtime.h"
#include <memory>
#include <stdexcept>

#ifndef MOD_ALLES
#error "mod-alles requires the public MOD_ALLES database definition"
#endif

namespace
{
    class AllesWorldScript final : public WorldScript
    {
    public:
        AllesWorldScript() : WorldScript("AllesWorldScript", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE,
            WORLDHOOK_ON_SHUTDOWN, WORLDHOOK_ON_AFTER_UNLOAD_ALL_MAPS}) { }

        ~AllesWorldScript() override { Alles::PublishRuntime(nullptr); }

        void OnStartup() override
        {
            Alles::CheckCharacterStatementLayout();
            bool const enabled = sConfigMgr->GetOption<bool>("Alles.Enable", false);
            if (!enabled)
            {
                LOG_INFO("module.alles", "Alles gameplay is disabled.");
                return;
            }
            try
            {
                auto settings = Alles::ReadRuntimeSettings();
                bool const external = settings.external;
                _runtime = std::make_unique<Alles::Runtime>(std::move(settings));
                LOG_INFO("module.alles", "Alles pilot enabled with {} interpretation.",
                    external ? "external bridge" : "in-process fake");
            }
            catch (std::exception const& error)
            {
                ABORT("Alles startup configuration failed: {}", error.what());
            }
            Alles::PublishRuntime(_runtime.get());
        }

        void OnUpdate(uint32 /*diff*/) override
        {
            if (_runtime)
                _runtime->Update();
        }

        void OnShutdown() override
        {
            if (_runtime)
                _runtime->BeginShutdown();
        }

        void OnAfterUnloadAllMaps() override
        {
            if (_runtime)
                _runtime->FinishShutdown();
        }

    private:
        std::unique_ptr<Alles::Runtime> _runtime;
    };

    class AllesPlayerScript final : public PlayerScript
    {
    public:
        AllesPlayerScript() : PlayerScript("AllesPlayerScript", {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_SAVE,
            PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_PLAYER_JUST_DIED}) { }

        void OnPlayerLogin(Player* player) override
        {
            if (auto* runtime = Alles::ActiveRuntime(); runtime && player)
                runtime->Login(*player);
        }

        void OnPlayerSave(Player* player) override
        {
            if (auto* runtime = Alles::ActiveRuntime(); runtime && player)
                runtime->Lifecycle(*player, Alles::IngressKind::Save);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (auto* runtime = Alles::ActiveRuntime(); runtime && player)
                runtime->Lifecycle(*player, Alles::IngressKind::Logout);
        }

        void OnPlayerJustDied(Player* player) override
        {
            if (auto* runtime = Alles::ActiveRuntime(); runtime && player)
                runtime->OwnDeath(*player);
        }
    };

    class AllesPacketScript final : public PlayerbotScript
    {
    public:
        AllesPacketScript() : PlayerbotScript("AllesPacketScript") { }

        void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
        {
            if (auto* runtime = Alles::ActiveRuntime(); runtime && player && packet)
                runtime->Packet(*player, *packet);
        }
    };

    class AllesUnitScript final : public UnitScript
    {
    public:
        AllesUnitScript() : UnitScript("AllesUnitScript", true, {UNITHOOK_ON_UNIT_DEATH}) { }

        void OnUnitDeath(Unit* victim, Unit* killer) override
        {
            if (auto* runtime = Alles::ActiveRuntime(); runtime && victim)
                runtime->WitnessDeath(*victim, killer);
        }
    };
}

void AddAllesCommandScripts();

void Addmod_allesScripts()
{
    Alles::CheckCharacterStatementLayout();
    new AllesWorldScript();
    new AllesPlayerScript();
    new AllesPacketScript();
    new AllesUnitScript();
    AddAllesCommandScripts();
}
