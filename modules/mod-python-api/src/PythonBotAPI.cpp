#include "PythonBotAPI.h"
#include "Transport.h"
#include "Config.h"
#include "Log.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldScript.h"
#include <iomanip>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

void AddPlayerbotsAdapterScripts();

namespace PythonAPI
{
namespace
{
constexpr std::size_t MaxBots = 64;
constexpr uint32 MovementPointId = 1;

std::string Quote(std::string const& value)
{
    std::ostringstream result;
    result << '"';
    for (unsigned char character : value)
    {
        if (character == '"' || character == '\\')
            result << '\\' << character;
        else if (character < 0x20)
            result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(character);
        else
            result << character;
    }
    result << '"';
    return result.str();
}

struct Events
{
    uint64 kills = 0;
    uint64 deaths = 0;
    uint64 levels = 0;
};

struct Bot
{
    BotBinding binding;
    uint64 registration = 0;
    uint64 owner = 0;
    uint64 episode = 0;
};

struct Pending
{
    Command command;
    uint64 registration;
    uint64 readyTick;
    uint32 spellResult = SPELL_CAST_OK;
};

class Bridge
{
public:
    Transport transport;
    std::unordered_map<ObjectGuid, Bot> bots;
    std::mutex eventMutex;
    std::unordered_map<ObjectGuid, Events> events;
    std::optional<Pending> pending;
    uint64 nextRegistration = 0;
    uint64 tick = 0;
    uint64 elapsed = 0;

    void Count(ObjectGuid guid, uint64 Events::* field, uint64 amount = 1)
    {
        std::lock_guard lock(eventMutex);
        auto found = events.find(guid);
        if (found != events.end())
            found->second.*field += amount;
    }

    void Release(ObjectGuid guid, Bot& bot)
    {
        if (!bot.owner)
            return;
        if (bot.binding.isAvailable && !bot.binding.isAvailable())
        {
            bot.owner = 0;
            return;
        }
        if (Player* player = ObjectAccessor::FindPlayer(guid))
        {
            player->StopMoving();
            player->GetMotionMaster()->Clear();
        }
        bot.binding.setControlled(false);
        bot.owner = 0;
    }

    std::string Envelope(Command const& command, bool ok)
    {
        return "{\"version\":1,\"id\":" + Quote(std::to_string(command.id))
            + ",\"ok\":" + (ok ? "true" : "false")
            + ",\"world_tick\":" + Quote(std::to_string(tick))
            + ",\"elapsed_ms\":" + Quote(std::to_string(elapsed));
    }

    void Error(Command const& command, char const* reason)
    {
        transport.Reply(command.client, Envelope(command, false) + ",\"error\":" + Quote(reason) + "}");
    }

    std::string Observation(ObjectGuid guid, Player& player, Bot const& bot)
    {
        Events totals;
        {
            std::lock_guard lock(eventMutex);
            totals = events.at(guid);
        }
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(9);
        output << "{\"guid\":" << Quote(std::to_string(guid.GetRawValue()))
            << ",\"name\":" << Quote(player.GetName())
            << ",\"registration\":" << Quote(std::to_string(bot.registration))
            << ",\"episode\":" << Quote(std::to_string(bot.episode))
            << ",\"health\":" << player.GetHealth() << ",\"max_health\":" << player.GetMaxHealth()
            << ",\"power\":" << player.GetPower(player.getPowerType())
            << ",\"max_power\":" << player.GetMaxPower(player.getPowerType())
            << ",\"power_type\":" << uint32(player.getPowerType())
            << ",\"level\":" << uint32(player.GetLevel())
            << ",\"map\":" << player.GetMapId() << ",\"instance\":" << player.GetInstanceId()
            << ",\"position\":[" << player.GetPositionX() << ',' << player.GetPositionY() << ','
            << player.GetPositionZ() << ',' << player.GetOrientation() << ']'
            << ",\"alive\":" << (player.IsAlive() ? "true" : "false")
            << ",\"combat\":" << (player.IsInCombat() ? "true" : "false")
            << ",\"casting\":" << (player.HasUnitState(UNIT_STATE_CASTING) ? "true" : "false")
            << ",\"events\":{\"kills\":" << Quote(std::to_string(totals.kills))
            << ",\"deaths\":" << Quote(std::to_string(totals.deaths))
            << ",\"levels\":" << Quote(std::to_string(totals.levels)) << "}}";
        return output.str();
    }

    void Complete()
    {
        if (!pending)
            return;
        Command const& command = pending->command;
        if (transport.Client() != command.client)
        {
            pending.reset();
            return;
        }
        ObjectGuid guid(command.bot);
        auto found = bots.find(guid);
        if (found == bots.end() || found->second.registration != pending->registration
            || found->second.owner != command.client
            || (found->second.binding.isAvailable && !found->second.binding.isAvailable()))
        {
            Error(command, "bot_unregistered");
            pending.reset();
            return;
        }
        Bot& bot = found->second;
        if (tick < pending->readyTick)
            return;
        if (command.operation == "reset" && !bot.binding.resetReady())
            return;
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        {
            if (command.operation == "reset")
                return;
            Error(command, "bot_unavailable");
            pending.reset();
            return;
        }
        if (command.operation == "reset")
        {
            if (!player->IsAlive())
            {
                Error(command, "reset_left_bot_dead");
                pending.reset();
                return;
            }
            ++bot.episode;
        }
        transport.Reply(command.client, Envelope(command, true)
            + ",\"spell_result\":" + std::to_string(pending->spellResult)
            + ",\"observation\":" + Observation(guid, *player, bot) + "}");
        pending.reset();
    }

    void Execute(Command const& command)
    {
        if (command.client != transport.Client())
            return;
        if (pending)
        {
            Error(command, "request_pending");
            return;
        }
        if (command.operation == "list")
        {
            std::string response = Envelope(command, true) + ",\"bots\":[";
            bool first = true;
            for (auto const& [guid, bot] : bots)
            {
                if (bot.binding.isAvailable && !bot.binding.isAvailable())
                    continue;
                if (!first)
                    response += ',';
                first = false;
                Player* player = ObjectAccessor::FindConnectedPlayer(guid);
                response += "{\"guid\":" + Quote(std::to_string(guid.GetRawValue()))
                    + ",\"name\":" + Quote(player ? player->GetName() : "")
                    + ",\"reset_supported\":" + (bot.binding.beginReset ? "true" : "false") + "}";
            }
            transport.Reply(command.client, response + "]}");
            return;
        }
        ObjectGuid guid(command.bot);
        auto found = bots.find(guid);
        if (found == bots.end())
        {
            Error(command, "bot_not_registered");
            return;
        }
        Bot& bot = found->second;
        if (bot.binding.isAvailable && !bot.binding.isAvailable())
        {
            Error(command, "bot_unregistered");
            return;
        }
        if (command.operation == "release")
        {
            if (bot.owner != command.client)
            {
                Error(command, "claim_required");
                return;
            }
            Release(guid, bot);
            transport.Reply(command.client, Envelope(command, true) + "}");
            return;
        }
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        {
            Error(command, "bot_unavailable");
            return;
        }
        if (command.operation == "claim")
        {
            if (!bot.owner && bot.binding.setControlled(true))
                bot.owner = command.client;
        }
        if (bot.owner != command.client)
        {
            Error(command, "claim_required");
            return;
        }

        uint32 spellResult = SPELL_CAST_OK;
        if (command.operation == "reset")
        {
            if (!bot.binding.beginReset || !bot.binding.beginReset())
            {
                Error(command, "reset_refused");
                return;
            }
            // Never dereference player after a provider operation that can remove it from the map.
        }
        else if (command.operation == "stop")
        {
            player->StopMoving();
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MoveIdle();
        }
        else if (command.operation == "move_to")
        {
            if (!player->IsAlive() || !MapMgr::IsValidMapCoord(player->GetMapId(), command.x, command.y, command.z)
                || player->GetDistance(command.x, command.y, command.z) > 50.0f)
            {
                Error(command, "invalid_destination");
                return;
            }
            player->GetMotionMaster()->MovePoint(MovementPointId, command.x, command.y, command.z,
                FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, false);
        }
        else if (command.operation == "cast")
        {
            Unit* target = ObjectAccessor::GetUnit(*player, ObjectGuid(command.target));
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(command.spell);
            if (!player->IsAlive() || !player->HasSpell(command.spell) || !target || !spell || spell->IsPassive())
            {
                Error(command, "invalid_spell_or_target");
                return;
            }
            spellResult = player->CastSpell(target, command.spell, false);
        }
        pending = Pending{command, bot.registration, tick + command.waitTicks, spellResult};
    }

    void Update(uint32 diff)
    {
        ++tick;
        elapsed += diff;
        uint64 client = transport.Client();
        for (auto& [guid, bot] : bots)
            if (bot.owner && bot.owner != client)
                Release(guid, bot);
        Complete();
        for (Command const& command : transport.Drain())
            Execute(command);
    }

    void Shutdown()
    {
        transport.Stop();
        pending.reset();
        for (auto& [guid, bot] : bots)
            Release(guid, bot);
        bots.clear();
        std::lock_guard lock(eventMutex);
        events.clear();
    }
};

Bridge& State()
{
    static Bridge bridge;
    return bridge;
}

class PythonAPIWorldScript : public WorldScript
{
public:
    PythonAPIWorldScript() : WorldScript("PythonAPIWorldScript") { }

    void OnStartup() override
    {
        if (!sConfigMgr->GetOption<bool>("PythonAPI.Enable", false))
            return;
        uint32 port = sConfigMgr->GetOption<uint32>("PythonAPI.Port", 5001);
        if (!port || port > 65535)
        {
            LOG_ERROR("module.pythonapi", "PythonAPI.Port must be 1..65535");
            return;
        }
        if (State().transport.Start(uint16(port)))
            LOG_INFO("module.pythonapi", "Python API listening on 127.0.0.1:{}", port);
    }

    void OnUpdate(uint32 diff) override { State().Update(diff); }
    void OnShutdown() override { State().Shutdown(); }
};

class PythonAPIPlayerScript : public PlayerScript
{
public:
    PythonAPIPlayerScript() : PlayerScript("PythonAPIPlayerScript") { }
    void OnPlayerCreatureKill(Player* player, Creature*) override
    {
        State().Count(player->GetGUID(), &Events::kills);
    }
    void OnPlayerJustDied(Player* player) override
    {
        State().Count(player->GetGUID(), &Events::deaths);
    }
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (player->GetLevel() > oldLevel)
            State().Count(player->GetGUID(), &Events::levels, player->GetLevel() - oldLevel);
    }
};
}

bool RegisterBot(ObjectGuid guid, BotBinding binding)
{
    Bridge& bridge = State();
    if (!guid.IsPlayer() || !guid || !binding.setControlled || bridge.bots.count(guid)
        || bridge.bots.size() >= MaxBots || bool(binding.beginReset) != bool(binding.resetReady))
        return false;
    bridge.bots.emplace(guid, Bot{std::move(binding), ++bridge.nextRegistration, 0, 0});
    std::lock_guard lock(bridge.eventMutex);
    bridge.events.emplace(guid, Events{});
    return true;
}

void UnregisterBot(ObjectGuid guid)
{
    Bridge& bridge = State();
    auto found = bridge.bots.find(guid);
    if (found == bridge.bots.end())
        return;
    bridge.Release(guid, found->second);
    bridge.bots.erase(found);
    std::lock_guard lock(bridge.eventMutex);
    bridge.events.erase(guid);
}

void AddScripts()
{
    new PythonAPIWorldScript();
    new PythonAPIPlayerScript();
}
}

void Addmod_python_apiScripts()
{
    PythonAPI::AddScripts();
    AddPlayerbotsAdapterScripts();
}
