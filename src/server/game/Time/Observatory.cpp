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

#include "Observatory.h"
#include "Config.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "SimulationBudget.h"
#include "SimulationClock.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr uint32 StepMs = SimulationBudget::StepMs;
    constexpr std::size_t MaxEvents = 65536;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> events;
    std::string latest;
    std::string directory;
    std::string runId;
    std::thread writer;
    std::atomic<bool> stopping{false};
    std::atomic<bool> fault{false};
    std::string faultReason = "journal_failure";
    struct Control
    {
        uint64 sequence = 0;
        double speed = 1;
        bool paused = false;
        uint32 bots = 100;
        uint32 observerMode = Observatory::OBSERVER_LOCKED;
    };
    Control pending;
    std::atomic<uint32> expectedBots{100};
    uint32 maxBots = 100;
    bool populationSettled = false;
    bool allowObservers = false;
    std::atomic<bool> observerSupport{false};
    std::atomic<bool> observerAdmissionOpen{false};
    // Applied by the world thread; read by packet and chat gates on the session's world update.
    std::atomic<uint32> observerMode{Observatory::OBSERVER_LOCKED};
    // Identity-only leases; never dereference these pointers. WorldSession destruction releases its lease.
    std::set<WorldSession const*> observerSessions;
    std::string controlError;
    bool trace = false;
    bool baseline = false;
    uint64 durationMs = 0;
    // Zero keeps one growing file per journal. Otherwise a journal is renamed to NAME.NNNNNN.ndjson once it
    // holds this many bytes so the bridge can retain a bounded recent window on disk and prune older segments.
    uint64 segmentBytes = 0;
    std::set<std::string> trackedUnits;
    uint64 eventSequence = 0;
    thread_local std::string context;
    // Ordinary-realm tap (see Observatory.h). Read on every Event() call, so it stays lock-free.
    std::atomic<Observatory::LiveSink> liveSink{nullptr};
    std::function<void()> agentMaintenance;
    std::function<std::string()> agentStatus;
    struct Totals
    {
        uint64 lastAiMs = 0;
        uint64 aiUpdates = 0;
        uint64 xp = 0;
        uint64 deaths = 0;
        uint64 quests = 0;
    };
    std::map<std::string, Totals> totals;
    std::set<std::string> cohort;
    std::set<uint32> configuredBots;

    std::string Quote(std::string_view input)
    {
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<float>::max_digits10);
        out << '"';
        for (unsigned char c : input)
        {
            if (c == '"' || c == '\\')
                out << '\\' << c;
            else if (c < 32)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
            else
                out << c;
        }
        out << '"';
        return out.str();
    }

    void WriteAtomic(std::string const& name, std::string const& value)
    {
        auto path = std::filesystem::path(directory) / name;
        std::ofstream out(path.string() + ".tmp", std::ios::trunc);
        out << value << '\n';
        out.close();
        if (!out)
            throw std::runtime_error("Observatory output failed");
        std::filesystem::rename(path.string() + ".tmp", path);
    }

    // An append-only NDJSON journal that is rotated into numbered segments once it reaches segmentBytes.
    struct Journal
    {
        std::string name;
        std::ofstream stream;
        uint64 bytes = 0;
        uint32 segments = 0;

        explicit Journal(std::string const& fileName) : name(fileName), stream(std::filesystem::path(directory) / name)
        {
        }

        void Append(std::string const& line)
        {
            stream << line << '\n';
            bytes += line.size() + 1;
        }

        // Only whole lines are ever moved: rotation happens after a flushed batch, never inside one.
        void Flush()
        {
            stream.flush();
            if (!stream)
                throw std::runtime_error("Observatory journal failed");
            if (!segmentBytes || bytes < segmentBytes)
                return;
            stream.close();
            std::ostringstream segment;
            segment << name.substr(0, name.size() - std::string_view(".ndjson").size()) << '.' << std::setw(6)
                    << std::setfill('0') << ++segments << ".ndjson";
            std::filesystem::rename(std::filesystem::path(directory) / name,
                std::filesystem::path(directory) / segment.str());
            stream.open(std::filesystem::path(directory) / name);
            bytes = 0;
            if (!stream)
                throw std::runtime_error("Observatory journal failed");
        }
    };

    void Writer()
    {
        try
        {
            Journal archive("snapshots.ndjson");
            Journal journal("events.ndjson");
            bool initialWritten = false;
            while (true)
            {
                std::deque<std::string> batch;
                std::string snapshot;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, 50ms, [] { return stopping.load() || !events.empty() || !latest.empty(); });
                    batch.swap(events);
                    snapshot.swap(latest);
                }
                for (auto const& event : batch)
                    journal.Append(event);
                if (!snapshot.empty())
                {
                    WriteAtomic("latest.json", snapshot);
                    if (!initialWritten && snapshot.find("\"ready\":true") != std::string::npos)
                    {
                        WriteAtomic("initial.json", snapshot);
                        initialWritten = true;
                    }
                    archive.Append(snapshot);
                }
                journal.Flush();
                archive.Flush();

                // Private local spool: bridge writes one atomic request. No gameplay pointers cross threads.
                std::ifstream control(std::filesystem::path(directory) / "control.txt");
                std::string requestedRun;
                uint64 sequence;
                double speed;
                uint32 paused;
                uint32 bots = 0;
                uint32 mode = 0;
                if (control >> requestedRun >> sequence >> speed >> paused >> bots)
                {
                    // Older bridges omit the observer mode; the current mode is then retained.
                    bool hasMode = bool(control >> mode);
                    if ((!baseline || (speed == 1 && paused == 0 && bots == pending.bots)) &&
                        bots <= maxBots && requestedRun == runId &&
                        SimulationBudget::IsValidSpeed(speed) && paused <= 1 &&
                        (!hasMode || mode <= Observatory::OBSERVER_FULL_GM))
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (sequence > pending.sequence)
                        {
                            controlError.clear();
                            if (!observerSessions.empty() && (speed != 1 || paused))
                            {
                                controlError = "GM POV requires 1x without pause until all observers disconnect";
                                pending.sequence = sequence;
                            }
                            else
                                pending = {sequence, speed, paused != 0, bots,
                                    hasMode ? mode : pending.observerMode};
                        }
                    }
                }
                if (stopping.load())
                    break;
            }
        }
        catch (std::exception const&)
        {
            fault.store(true);
        }
    }

    std::string Players()
    {
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<float>::max_digits10);
        out << '[';
        bool first = true;
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player->IsInWorld() || !player->GetSession() || !player->GetSession()->IsBot())
                continue;
            std::string id = guid.ToString();
            if (!first)
                out << ',';
            first = false;
            auto const& count = totals[id];
            out << "{\"id\":" << Quote(id) << ",\"name\":" << Quote(player->GetName())
                << ",\"map\":" << player->GetMapId() << ",\"instance\":" << player->GetInstanceId()
                << ",\"zone\":" << player->GetZoneId() << ",\"x\":" << player->GetPositionX()
                << ",\"y\":" << player->GetPositionY() << ",\"z\":" << player->GetPositionZ()
                << ",\"level\":" << unsigned(player->GetLevel()) << ",\"xp\":" << player->GetUInt32Value(PLAYER_XP)
                << ",\"nextLevelXp\":" << player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
                << ",\"health\":" << player->GetHealth() << ",\"maxHealth\":" << player->GetMaxHealth()
                << ",\"lastAiMs\":" << count.lastAiMs << ",\"aiUpdates\":" << count.aiUpdates
                << ",\"money\":" << player->GetMoney() << ",\"earnedXp\":" << count.xp << ",\"deaths\":" << count.deaths
                << ",\"questCompletions\":" << count.quests << ",\"activity\":"
                << Quote(!player->IsAlive()                   ? "dead"
                         : player->IsInCombat()               ? "combat"
                         : player->IsNonMeleeSpellCast(false) ? "casting"
                         : player->isMoving()                 ? "moving"
                                                              : "idle")
                << ",\"gear\":[";
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                if (slot != EQUIPMENT_SLOT_START)
                    out << ',';
                Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                out << (item ? item->GetEntry() : 0);
            }
            out << "],\"quests\":[";
            bool firstQuest = true;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 quest = player->GetQuestSlotQuestId(slot);
                if (!quest)
                    continue;
                if (!firstQuest)
                    out << ',';
                firstQuest = false;
                out << "{\"id\":" << quest << ",\"state\":" << player->GetQuestSlotState(slot) << ",\"objectives\":[";
                for (uint8 objective = 0; objective < 4; ++objective)
                {
                    if (objective)
                        out << ',';
                    out << player->GetQuestSlotCounter(slot, objective);
                }
                out << "],\"items\":[";
                auto const& statuses = player->getQuestStatusMap();
                auto status = statuses.find(quest);
                for (uint8 objective = 0; objective < QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
                {
                    if (objective)
                        out << ',';
                    out << (status != statuses.end() ? status->second.ItemCount[objective] : 0);
                }
                out << "]}";
            }
            out << "]}";
        }
        out << ']';
        return out.str();
    }
} // namespace

bool Observatory::Initialize()
{
    if (!sConfigMgr->GetOption<bool>("Observatory.Enable", false))
        return true;

    if (sConfigMgr->GetOption<std::string>("Observatory.DisposableAcknowledgement", "") != "DISPOSABLE_BOTS_ONLY")
        return false;
    // Check before opening any database: persisted virtual epochs are unsuitable for production or restart.
    for (std::string const key :
         {"LoginDatabaseInfo", "WorldDatabaseInfo", "CharacterDatabaseInfo", "PlayerbotsDatabaseInfo"})
    {
        std::string info = sConfigMgr->GetOption<std::string>(key, "");
        auto separator = info.rfind(';');
        if (separator == std::string::npos || info.substr(separator + 1).rfind("obs_", 0) != 0)
            return false;
    }
    expectedBots = sConfigMgr->GetOption<uint32>("Observatory.BotCount", 100);
    maxBots = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 0);
    if (!maxBots || maxBots > 100 || expectedBots > maxBots ||
        !sConfigMgr->GetOption<bool>("AiPlayerbot.Enabled", false) ||
        !sConfigMgr->GetOption<bool>("AiPlayerbot.RandomBotAutologin", false) ||
        sConfigMgr->GetOption<bool>("AiPlayerbot.DisabledWithoutRealPlayer", true) ||
        sConfigMgr->GetOption<bool>("AiPlayerbot.EnablePeriodicOnlineOffline", true) ||
        sConfigMgr->GetOption<uint32>("AiPlayerbot.MinRandomBots", 0) != maxBots ||
        sConfigMgr->GetOption<uint32>("AiPlayerbot.CommandServerPort", 8888) != 0 ||
        sConfigMgr->GetOption<bool>("SOAP.Enabled", false) || sConfigMgr->GetOption<bool>("Ra.Enable", false) ||
        sConfigMgr->GetOption<bool>("Console.Enable", true))
        return false;

    baseline = sConfigMgr->GetOption<bool>("Observatory.RealTimeBaseline", false);
    allowObservers = sConfigMgr->GetOption<bool>("Observatory.AllowGmObservers", false);
    pending.observerMode = std::min(sConfigMgr->GetOption<uint32>("Observatory.ObserverMode", OBSERVER_LOCKED),
        uint32(OBSERVER_FULL_GM));
    observerMode.store(pending.observerMode);
    if (baseline && allowObservers)
        return false;
    durationMs = sConfigMgr->GetOption<uint64>("Observatory.DurationMs", 0);
    if (durationMs % StepMs != 0)
        return false;
    trace = sConfigMgr->GetOption<bool>("Observatory.Trace", false);
    segmentBytes = sConfigMgr->GetOption<uint64>("Observatory.JournalSegmentBytes", 0);
    if (segmentBytes && segmentBytes < 1024 * 1024)
        return false; // Segments under 1 MiB would scatter a traced run over thousands of files.
    std::string identities = sConfigMgr->GetOption<std::string>("Observatory.BotGuids", "");
    std::istringstream identityStream(identities);
    std::string identity;
    while (std::getline(identityStream, identity, ','))
    {
        uint32 id = 0;
        auto [end, error] = std::from_chars(identity.data(), identity.data() + identity.size(), id);
        if (error != std::errc() || end != identity.data() + identity.size() || !id ||
            !configuredBots.insert(id).second)
            return false;
    }
    if (!configuredBots.empty())
    {
        if (configuredBots.size() < expectedBots || configuredBots.size() > maxBots)
            return false;
        maxBots = configuredBots.size();
    }
    pending.bots = expectedBots;

    directory = sConfigMgr->GetOption<std::string>("Observatory.Directory", "");
    try
    {
        if (directory.empty() || !std::filesystem::create_directory(directory))
            return false; // A run never reuses a spool, journal, or stale control file.
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        runId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::ostringstream manifest;
        manifest << "{\"schema\":1,\"run\":" << Quote(runId) << ",\"stepMs\":" << StepMs
                 << ",\"expectedBots\":" << expectedBots << ",\"settings\":{";
        bool first = true;
        for (auto const& key : sConfigMgr->GetKeysByString(""))
        {
            // Record simulation/gameplay settings only; connection strings and credentials stay private.
            if (key.rfind("AiPlayerbot.", 0) != 0 && key.rfind("Rate.", 0) != 0 && key.rfind("Observatory.", 0) != 0 &&
                key != "MapUpdateInterval" && key != "MapUpdate.Threads")
                continue;
            if (key.find("Password") != std::string::npos || key.find("Token") != std::string::npos)
                continue;
            if (!first)
                manifest << ',';
            first = false;
            manifest << Quote(key) << ':' << Quote(sConfigMgr->GetOption<std::string>(key, "", false));
        }
        manifest << "}}";
        WriteAtomic("manifest.json", manifest.str());
        SimulationClock::Enable(!baseline);
        writer = std::thread(Writer);
        return true;
    }
    catch (std::exception const&)
    {
        return false;
    }
}

bool Observatory::AllowsBot(uint32 characterId)
{
    return !SimulationClock::Enabled() || configuredBots.empty() || configuredBots.contains(characterId);
}

bool Observatory::AllowsObservers()
{
    return SimulationClock::Enabled() && allowObservers && observerSupport.load();
}

void Observatory::ObserverSupportReady()
{
    observerSupport.store(true);
}

bool Observatory::RegisterObserver(WorldSession const* session)
{
    if (!AllowsObservers() || !session || session->IsBot() || session->GetSecurity() < SEC_GAMEMASTER)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!observerAdmissionOpen.load() || fault.load() || stopping.load())
            return false;
        if (!observerSessions.insert(session).second)
            return false;
        pending.speed = 1;
        pending.paused = false;
        controlError.clear();
    }
    Event(nullptr, "observer_connect", session->GetAccountId(), "GM POV; speed locked to 1x");
    return true;
}

void Observatory::UnregisterObserver(WorldSession const* session)
{
    if (!AllowsObservers())
        return;
    bool removed;
    {
        std::lock_guard<std::mutex> lock(mutex);
        removed = observerSessions.erase(session) != 0;
    }
    if (removed)
        Event(nullptr, "observer_disconnect", session->GetAccountId(),
            "1x retained; controls unlock after last observer");
}

bool Observatory::IsObserver(WorldSession const* session)
{
    if (!AllowsObservers() || !session || session->IsBot())
        return false;
    std::lock_guard<std::mutex> lock(mutex);
    return observerSessions.contains(session);
}

uint32 Observatory::ObserverMode()
{
    return AllowsObservers() ? observerMode.load() : uint32(OBSERVER_LOCKED);
}

bool Observatory::AllowsObserverOpcode(uint32 opcode)
{
    uint32 mode = observerMode.load();
    if (mode >= OBSERVER_FULL_GM)
        return true;
    if (mode >= OBSERVER_ROAM)
    {
        // Client-driven movement of the observer's own character plus the acknowledgements the core requests.
        switch (opcode)
        {
            case MSG_MOVE_START_FORWARD:
            case MSG_MOVE_START_BACKWARD:
            case MSG_MOVE_STOP:
            case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT:
            case MSG_MOVE_STOP_STRAFE:
            case MSG_MOVE_JUMP:
            case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT:
            case MSG_MOVE_STOP_TURN:
            case MSG_MOVE_START_PITCH_UP:
            case MSG_MOVE_START_PITCH_DOWN:
            case MSG_MOVE_STOP_PITCH:
            case MSG_MOVE_SET_RUN_MODE:
            case MSG_MOVE_SET_WALK_MODE:
            case MSG_MOVE_FALL_LAND:
            case MSG_MOVE_START_SWIM:
            case MSG_MOVE_STOP_SWIM:
            case MSG_MOVE_START_ASCEND:
            case MSG_MOVE_STOP_ASCEND:
            case MSG_MOVE_START_DESCEND:
            case MSG_MOVE_SET_FACING:
            case MSG_MOVE_SET_PITCH:
            case MSG_MOVE_HEARTBEAT:
            case CMSG_MOVE_TIME_SKIPPED:
            case CMSG_MOVE_FALL_RESET:
            case CMSG_MOVE_CHNG_TRANSPORT:
            case CMSG_MOVE_SPLINE_DONE:
            case CMSG_SET_ACTIVE_MOVER:
            case CMSG_MOVE_NOT_ACTIVE_MOVER:
            case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:
            case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:
            case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_TURN_RATE_CHANGE_ACK:
            case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:
            case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_PITCH_RATE_CHANGE_ACK:
            case CMSG_FORCE_MOVE_ROOT_ACK:
            case CMSG_FORCE_MOVE_UNROOT_ACK:
            case CMSG_MOVE_KNOCK_BACK_ACK:
            case CMSG_MOVE_HOVER_ACK:
            case CMSG_MOVE_FEATHER_FALL_ACK:
            case CMSG_MOVE_WATER_WALK_ACK:
            case CMSG_MOVE_SET_CAN_FLY_ACK:
            case CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK:
            case CMSG_MOVE_GRAVITY_DISABLE_ACK:
            case CMSG_MOVE_GRAVITY_ENABLE_ACK:
            case CMSG_MOVE_SET_COLLISION_HGT_ACK:
            case CMSG_ZONEUPDATE:
            case CMSG_AREATRIGGER:
            case CMSG_STANDSTATECHANGE:
            case CMSG_MOUNTSPECIAL_ANIM:
                return true;
            default:
                break;
        }
    }
    // Locked: only session/UI bookkeeping, readonly queries, POV chat and server teleport acknowledgements.
    switch (opcode)
    {
        case CMSG_CHAR_ENUM:
        case CMSG_CHAR_CREATE:
        case CMSG_PLAYER_LOGIN:
        case CMSG_LOGOUT_REQUEST:
        case CMSG_LOGOUT_CANCEL:
        case CMSG_NAME_QUERY:
        case CMSG_QUERY_TIME:
        case CMSG_CREATURE_QUERY:
        case CMSG_GAMEOBJECT_QUERY:
        case CMSG_ITEM_QUERY_SINGLE:
        case CMSG_PAGE_TEXT_QUERY:
        case CMSG_QUEST_QUERY:
        case CMSG_QUEST_POI_QUERY:
        case CMSG_REQUEST_ACCOUNT_DATA:
        case CMSG_UPDATE_ACCOUNT_DATA:
        case CMSG_READY_FOR_ACCOUNT_DATA_TIMES:
        case CMSG_TIME_SYNC_RESP:
        case CMSG_REALM_SPLIT:
        case CMSG_WARDEN_DATA:
        case CMSG_SET_SELECTION:
        case CMSG_MESSAGECHAT:
        case CMSG_CONTACT_LIST:
        case CMSG_NEXT_CINEMATIC_CAMERA:
        case CMSG_COMPLETE_CINEMATIC:
        case CMSG_REQUEST_RAID_INFO:
        case CMSG_GET_MIRRORIMAGE_DATA:
        case MSG_MOVE_WORLDPORT_ACK:
        case MSG_MOVE_TELEPORT_ACK:
            return true;
        default:
            return false;
    }
}

bool Observatory::AllowsObserverChat(uint32 type, uint32 lang, std::string const& msg)
{
    if (observerMode.load() >= OBSERVER_FULL_GM)
        return true;
    return type == CHAT_MSG_SAY && lang != LANG_ADDON && (msg == ".pov" || msg.rfind(".pov ", 0) == 0);
}

void Observatory::ObserverCommand(WorldSession const* session, std::string const& command)
{
    if (!session || !IsObserver(session))
        return;
    // Run-level provenance: any command a human GM runs is visible in the journal, POV included.
    Event(nullptr, "observer_command", session->GetAccountId(), command);
}

uint32 Observatory::TargetBotCount()
{
    return expectedBots;
}

void Observatory::PopulationSettled(bool settled)
{
    populationSettled = settled;
}

void Observatory::Fail(std::string_view reason)
{
    if (!SimulationClock::Enabled())
        return;
    std::lock_guard<std::mutex> lock(mutex);
    faultReason = reason;
    fault.store(true);
}

void Observatory::Stop()
{
    stopping.store(true);
    wake.notify_one();
    if (writer.joinable())
        writer.join();
}

void Observatory::SetAgentRuntimeHooks(std::function<void()> maintenance, std::function<std::string()> status)
{
    agentMaintenance = std::move(maintenance);
    agentStatus = std::move(status);
}

void Observatory::SetLiveSink(LiveSink sink)
{
    liveSink.store(sink, std::memory_order_release);
}

Observatory::Context::Context(std::string_view name)
{
    if (SimulationClock::Enabled() || liveSink.load(std::memory_order_relaxed))
    {
        _previous = context;
        context = name;
    }
}

Observatory::Context::~Context()
{
    if (SimulationClock::Enabled() || liveSink.load(std::memory_order_relaxed))
        context = _previous;
}

void Observatory::Event(Player const* player, std::string_view kind, uint64 value, std::string_view detail)
{
    if (player && (!player->GetSession() || !player->GetSession()->IsBot()))
        return;
    if (!SimulationClock::Enabled())
    {
        // No run, no journal: hand the record to the installed module sink, if any, on the raising thread.
        if (auto sink = liveSink.load(std::memory_order_acquire))
            sink(player, kind, value, detail, context);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto id = player ? player->GetGUID().ToString() : "";
    auto& count = totals[id];
    if (kind == "ai_update")
    {
        count.lastAiMs = SimulationClock::Elapsed().count();
        ++count.aiUpdates;
        return;
    }
    if (kind == "xp")
        count.xp += value;
    if (kind == "death")
        ++count.deaths;
    if (kind == "quest_reward")
        ++count.quests;
    if (events.size() >= MaxEvents)
    {
        fault.store(true); // Explicitly invalidate and freeze a run whose audit trail cannot keep up.
        return;
    }
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"run\":" << Quote(runId) << ",\"seq\":" << ++eventSequence
        << ",\"simMs\":" << SimulationClock::Elapsed().count() << ",\"bot\":" << Quote(id)
        << ",\"kind\":" << Quote(kind) << ",\"value\":" << value << ",\"detail\":" << Quote(detail)
        << ",\"context\":" << Quote(context) << ",\"map\":" << (player ? player->GetMapId() : 0)
        << ",\"instance\":" << (player ? player->GetInstanceId() : 0) << '}';
    events.push_back(out.str());
}

void Observatory::Probe(Unit const* actor, std::string_view kind, uint64 value, uint32 spell, Unit const* other)
{
    if (!SimulationClock::Enabled() || !trace || !actor)
        return;
    auto isBot = [](Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        return player && player->GetSession() && player->GetSession()->IsBot();
    };
    std::lock_guard<std::mutex> lock(mutex);
    auto id = actor->GetGUID().ToString();
    if (!isBot(actor) && !isBot(other) && !trackedUnits.contains(id))
        return;
    trackedUnits.insert(id);
    if (other)
        trackedUnits.insert(other->GetGUID().ToString());
    if (events.size() >= MaxEvents)
    {
        fault.store(true);
        return;
    }
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"run\":" << Quote(runId) << ",\"seq\":" << ++eventSequence
        << ",\"simMs\":" << SimulationClock::Elapsed().count() << ",\"bot\":" << Quote(id)
        << ",\"kind\":" << Quote(kind) << ",\"value\":" << value << ",\"spell\":" << spell
        << ",\"other\":" << Quote(other ? other->GetGUID().ToString() : "") << ",\"context\":" << Quote(context)
        << ",\"map\":" << actor->GetMapId() << ",\"instance\":" << actor->GetInstanceId()
        << ",\"x\":" << actor->GetPositionX() << ",\"y\":" << actor->GetPositionY()
        << ",\"z\":" << actor->GetPositionZ() << ",\"health\":" << actor->GetHealth() << '}';
    events.push_back(out.str());
}

void Observatory::Run()
{
    auto start = Clock::now();
    auto previous = start;
    auto sampled = start;
    auto lastWorldTick = start;
    uint64 readyAt = 0;
    bool completed = false;
    uint64 sampledSim = 0;
    uint64 snapshotSequence = 0;
    SimulationBudget budget;
    uint64 maxTickUs = 0;
    Control applied;
    applied.bots = expectedBots;
    uint64 populationSince = 0;
    std::string problem;
    bool ready = false;
    auto nextObserverTick = Clock::now();
    observerAdmissionOpen.store(true);
    while (!World::IsStopped())
    {
        ++World::m_worldLoopCounter; // The real-time watchdog sees a heartbeat even while paused.
        auto now = Clock::now();
        auto realUs = std::chrono::duration_cast<Microseconds>(now - previous).count();
        previous = now;
        completed = ready && durationMs && SimulationClock::Elapsed().count() >= int64(readyAt + durationMs);
        budget.Accrue(uint64(realUs), applied.speed, applied.paused || fault.load() || !problem.empty() || completed);
        bool observing;
        {
            std::lock_guard<std::mutex> lock(mutex);
            observing = !observerSessions.empty();
            applied = pending;
        }
        if (applied.observerMode != observerMode.load())
        {
            observerMode.store(applied.observerMode);
            Event(nullptr, "observer_mode", applied.observerMode, std::to_string(applied.sequence));
        }
        observerAdmissionOpen.store(!fault.load() && problem.empty() && !completed);
        if (applied.bots != expectedBots)
        {
            expectedBots = applied.bots;
            populationSettled = false;
            populationSince = SimulationClock::Elapsed().count();
            Event(nullptr, "population_target", expectedBots, std::to_string(applied.sequence));
        }
        if (agentMaintenance && (applied.paused || fault.load() || !problem.empty() || completed))
            agentMaintenance();
        // One normal-sized tick at a time. Debt is retained across speed changes, overload and pauses.
        if ((!observing || now >= nextObserverTick) &&
            budget.Consume(applied.paused || fault.load() || !problem.empty() || completed))
        {
            auto tickStart = Clock::now();
            // Retain accumulated debt, but never run a burst of catch-up steps under a native client.
            nextObserverTick = tickStart + Milliseconds(StepMs);
            uint32 diff =
                baseline ? uint32(std::chrono::duration_cast<Milliseconds>(tickStart - lastWorldTick).count()) : StepMs;
            lastWorldTick = tickStart;
            if (baseline)
                budget = SimulationBudget();
            SimulationClock::Advance(Milliseconds(diff));
            sWorld->Update(diff);
            maxTickUs =
                std::max(maxTickUs, uint64(std::chrono::duration_cast<Microseconds>(Clock::now() - tickStart).count()));
            if (trace)
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                    if (player->IsInWorld())
                        Probe(player, "position");
        }
        else
            std::this_thread::sleep_for(1ms);

        auto sampleNow = Clock::now();
        if (sampleNow - sampled < 250ms)
            continue;
        std::set<std::string> online;
        uint32 inWorld = 0;
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (player->GetSession() && !player->GetSession()->IsBot() && !IsObserver(player->GetSession()))
                problem = "human_session";
            if (player->GetSession() && player->GetSession()->IsBot())
            {
                online.insert(guid.ToString()); // Include bots temporarily between maps.
                if (player->IsInWorld())
                    ++inWorld;
            }
        }
        bool populationReady = populationSettled && inWorld == expectedBots && online.size() == expectedBots;
        uint64 sim = SimulationClock::Elapsed().count();
        if (populationReady)
            populationSince = sim;
        else if (sim - populationSince > 600000)
            problem = "population_timeout";
        for (auto const& id : online)
            if (!cohort.contains(id))
                Event(nullptr, "population_join", 0, id);
        for (auto const& id : cohort)
            if (!online.contains(id))
                Event(nullptr, "population_leave", 0, id);
        cohort = online;
        if (!ready && populationReady)
        {
            ready = true;
            readyAt = SimulationClock::Elapsed().count();
        }
        double realMs = std::chrono::duration<double, std::milli>(sampleNow - sampled).count();
        double achieved = (sim - sampledSim) / realMs;
        std::lock_guard<std::mutex> lock(mutex);
        uint32 activeBots = 0;
        for (auto const& id : online)
            if (totals[id].aiUpdates && sim - totals[id].lastAiMs < 10000)
                ++activeBots;
        Totals runTotals;
        for (auto const& [id, count] : totals)
        {
            runTotals.xp += count.xp;
            runTotals.quests += count.quests;
            runTotals.deaths += count.deaths;
        }
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<float>::max_digits10);
        out << "{\"schema\":1,\"run\":" << Quote(runId) << ",\"seq\":" << ++snapshotSequence << ",\"simMs\":" << sim
            << ",\"realMs\":" << std::chrono::duration_cast<Milliseconds>(sampleNow - start).count()
            << ",\"requestedSpeed\":" << applied.speed << ",\"achievedSpeed\":" << achieved
            << ",\"speedStep\":0.1"
            << ",\"baseline\":" << (baseline ? "true" : "false") << ",\"completed\":" << (completed ? "true" : "false")
            << ",\"readyAtMs\":" << readyAt << ",\"paused\":" << (applied.paused ? "true" : "false")
            << ",\"observersAllowed\":" << (AllowsObservers() ? "true" : "false")
            << ",\"observers\":" << observerSessions.size() << ",\"observerMode\":" << applied.observerMode
            << ",\"controlError\":" << Quote(controlError)
            << ",\"controlSeq\":" << applied.sequence << ",\"backlogMs\":" << budget.DebtMicroseconds() / 1000
            << ",\"maxTickUs\":" << maxTickUs << ",\"activeBots\":" << activeBots
            << ",\"overloaded\":" << (budget.DebtMicroseconds() > 1000000 ? "true" : "false")
            << ",\"ready\":" << (ready ? "true" : "false") << ",\"expectedBots\":" << expectedBots
            << ",\"maxBots\":" << maxBots << ",\"populationPending\":" << (populationReady ? "false" : "true")
            << ",\"runTotals\":{\"xp\":" << runTotals.xp << ",\"quests\":" << runTotals.quests
            << ",\"deaths\":" << runTotals.deaths << '}'
            << ",\"onlineBots\":" << online.size() << ",\"fault\":" << Quote(fault.load() ? faultReason : problem)
            << ",\"interpreter\":" << (agentStatus ? agentStatus() : "null")
            << ",\"bots\":" << Players() << '}';
        latest = out.str();
        maxTickUs = 0;
        sampled = sampleNow;
        sampledSim = sim;
        wake.notify_one();
    }
}
