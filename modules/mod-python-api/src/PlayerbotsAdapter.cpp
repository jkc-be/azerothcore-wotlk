#include "Config.h"
#include "Log.h"
#include "WorldScript.h"

#if __has_include("../../mod-playerbots/src/Bot/PlayerbotAI.h")
#include "../../mod-playerbots/src/Bot/PlayerbotAI.h"
#ifndef PLAYERBOTS_EXTERNAL_CONTROL_API
#error "Apply integrations/playerbots/playerbots-external-control.patch to mod-playerbots before building this adapter"
#endif

#include "PythonBotAPI.h"
#include "EventMap.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
enum AdapterEvents : uint32
{
    DiscoverBots = 1
};

struct Registration
{
    ObjectGuid guid;
    uint64 generation;
    WorldLocation origin;
    uint32 originInstance = 0;

    PlayerbotAI* Resolve() const
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || !player->GetSession() || player->GetSession()->isLogingOut())
            return nullptr;
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player);
        if (!ai || IsSelfBot(player) || ai->GetExternalControlGeneration() != generation)
            return nullptr;
        return ai;
    }

    bool Control(bool enabled)
    {
        PlayerbotAI* ai = Resolve();
        if (!ai)
            return !enabled;
        Player* player = ai->GetBot();
        if (enabled)
        {
            if (ai->IsExternallyControlled() || !player->IsInWorld() || player->IsBeingTeleported()
                || player->IsInFlight() || player->GetVehicle() || player->GetTransport())
                return false;
            origin = WorldLocation(player->GetMapId(), *player);
            originInstance = player->GetInstanceId();
        }
        ai->SetExternalControl(enabled);
        return true;
    }

    void ClearSelfbotOwnership()
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        PlayerbotAI* ai = player ? sPlayerbotsMgr.GetPlayerbotAI(player) : nullptr;
        if (ai && ai->GetExternalControlGeneration() == generation && IsSelfBot(player))
            ai->SetExternalControl(false);
    }

    bool BeginReset()
    {
        PlayerbotAI* ai = Resolve();
        if (!ai || !ai->IsExternallyControlled())
            return false;
        Player* player = ai->GetBot();
        // Character resets are supported on outdoor maps, with no vehicle/taxi/transport transfer.
        if (originInstance || !player->IsInWorld() || player->GetMap()->Instanceable()
            || player->IsInFlight() || player->GetVehicle() || player->GetTransport())
            return false;
        ai->Reset(true);
        player->CombatStopWithPets(true);
        player->AttackStop();
        player->StopMoving();
        player->GetMotionMaster()->Clear();
        if (player->isDead())
        {
            player->ResurrectPlayer(1.0f, false);
            player->SpawnCorpseBones();
        }
        if (!player->IsAlive())
            return false;
        player->RemoveAllAuras();
        player->RemoveAllSpellCooldown();
        player->SetHealth(player->GetMaxHealth());
        player->SetPower(player->getPowerType(), player->GetMaxPower(player->getPowerType()));
        return player->TeleportTo(origin);
    }

    bool ResetReady()
    {
        PlayerbotAI* ai = Resolve();
        if (!ai)
            return false;
        // Reuse Playerbots' handshake logic; its session loop also checks the same teleport flags.
        if (ai->GetBot()->IsBeingTeleported())
            ai->HandleTeleportAck();
        ai = Resolve();
        if (!ai)
            return false;
        Player* player = ai->GetBot();
        return player->IsAlive() && player->IsInWorld() && !player->IsBeingTeleported()
            && player->GetMapId() == origin.GetMapId() && player->GetInstanceId() == originInstance
            && player->GetExactDist(origin) < 1.0f;
    }
};

class PlayerbotsPythonAdapter : public WorldScript
{
public:
    PlayerbotsPythonAdapter() : WorldScript("PlayerbotsPythonAdapter") { }

    void OnStartup() override
    {
        if (!sConfigMgr->GetOption<bool>("PythonAPI.Enable", false))
            return;
        std::string configured = sConfigMgr->GetOption<std::string>("PythonAPI.Playerbots.Names", "");
        std::replace(configured.begin(), configured.end(), ',', ' ');
        std::istringstream input(configured);
        std::string name;
        while (input >> name && _names.size() < 64)
            _names.push_back(name);
        _events.ScheduleEvent(DiscoverBots, Milliseconds(1));
        LOG_INFO("module.pythonapi", "Playerbots adapter enabled for {} configured names", _names.size());
    }

    void OnUpdate(uint32 diff) override
    {
        _events.Update(diff);
        if (_events.ExecuteEvent() != DiscoverBots)
            return;
        for (auto it = _registrations.begin(); it != _registrations.end();)
        {
            if (!it->second->Resolve())
            {
                it->second->ClearSelfbotOwnership();
                PythonAPI::UnregisterBot(it->first);
                it = _registrations.erase(it);
            }
            else
                ++it;
        }
        for (std::string const& name : _names)
        {
            Player* player = ObjectAccessor::FindPlayerByName(name);
            if (!player || _registrations.count(player->GetGUID()))
                continue;
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player);
            if (!ai || IsSelfBot(player))
                continue;
            auto registration = std::make_shared<Registration>();
            registration->guid = player->GetGUID();
            registration->generation = ai->GetExternalControlGeneration();
            PythonAPI::BotBinding binding;
            binding.setControlled = [registration](bool enabled) { return registration->Control(enabled); };
            binding.beginReset = [registration] { return registration->BeginReset(); };
            binding.resetReady = [registration] { return registration->ResetReady(); };
            binding.isAvailable = [registration] { return registration->Resolve() != nullptr; };
            if (PythonAPI::RegisterBot(registration->guid, std::move(binding)))
            {
                _registrations.emplace(registration->guid, registration);
                LOG_INFO("module.pythonapi", "Registered Playerbot {} ({})", name, player->GetGUID().ToString());
            }
        }
        _events.ScheduleEvent(DiscoverBots, Milliseconds(1000));
    }

    void OnShutdown() override
    {
        for (auto const& [guid, registration] : _registrations)
        {
            registration->ClearSelfbotOwnership();
            PythonAPI::UnregisterBot(guid);
        }
        _registrations.clear();
        _events.Reset();
    }

private:
    EventMap _events;
    std::vector<std::string> _names;
    std::unordered_map<ObjectGuid, std::shared_ptr<Registration>> _registrations;
};
}

void AddPlayerbotsAdapterScripts()
{
    new PlayerbotsPythonAdapter();
}

#else

namespace
{
class MissingPlayerbotsAdapter : public WorldScript
{
public:
    MissingPlayerbotsAdapter() : WorldScript("MissingPlayerbotsAdapter") { }
    void OnStartup() override
    {
        if (sConfigMgr->GetOption<bool>("PythonAPI.Enable", false))
            LOG_WARN("module.pythonapi", "Playerbots adapter unavailable: mod-playerbots was absent at build time");
    }
};
}

void AddPlayerbotsAdapterScripts()
{
    new MissingPlayerbotsAdapter();
}

#endif
