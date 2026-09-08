/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Runtime.h"
#include "ChatSender.h"
#include "ConversationRuntime.h"
#include "ObjectiveRuntime.h"
#include "DataMap.h"
#include "Bag.h"
#include "CryptoRandom.h"
#include "Errors.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectVisibilityContainer.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "interpreter/PilotCoordinator.h"
#include "perception/LiveCapture.h"
#include "storage/SnapshotDao.h"
#include "telemetry/Recorder.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <map>

namespace Alles
{
namespace
{
std::atomic<Runtime*> PublishedRuntime{nullptr};
std::atomic<uint64_t> NextAttachment{1};
std::string const AttachmentKey = "mod-alles.attachment";

struct Attachment final : DataMap::Base
{
    explicit Attachment(uint64_t value) : token(value) { }
    uint64_t const token;
};

ActorKey Owner(Player const& player)
{
    return {ActorKind::Player, player.GetGUID().GetCounter()};
}

uint64_t Token(Player const& player)
{
    auto const* attachment = player.CustomData.Get<Attachment>(AttachmentKey);
    return attachment ? attachment->token : 0;
}

uint64_t RealNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t GameNow()
{
    auto const now = GameTime::GetSystemTime();
    // Startup precedes the first cached millisecond clock update. Persist epoch game time, never server uptime.
    if (now == SystemTimePoint::min())
        return uint64_t(GameTime::GetGameTime().count()) * 1000;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

bool EligibleSpeaker(Player& player)
{
    if (!player.IsInWorld() || !player.IsAlive() || player.IsInCombat() || player.IsBeingTeleported()
        || !player.GetSession() || !player.GetSession()->IsBot() || player.GetSession()->isLogingOut())
        return false;
    auto* ai = sPlayerbotsMgr.GetPlayerbotAI(&player);
    return ai && !IsSelfBot(&player) && !ai->IsExternallyControlled();
}
}

// Named outside Impl, whose Telemetry() member would otherwise shadow the namespace.
using TelemetryRecorder = Telemetry::Recorder;

struct Runtime::Impl
{
    struct OwnerRuntime
    {
        bool loadInFlight = false;
        uint64_t retryLoadMs = 0;
        uint64_t lastSpeechMs = 0;
        std::deque<std::pair<std::string, uint64_t>> spoken;
        std::string question;
        uint64_t questionTimeMs = 0;
        std::set<ObjectGuid> visible;
        std::map<ObjectGuid, uint64_t> metCooldown;
    };

    explicit Impl(RuntimeSettings value) : settings(std::move(value)), ingress(settings.ingressCapacity),
        store(settings.limits, settings.memory), dao(settings.limits, settings.memory),
        coordinator(store, settings.memory), thread(std::this_thread::get_id())
    {
        for (auto const owner : settings.owners)
        {
            if (!store.Activate(owner, 0) || !coordinator.Track(owner))
                ABORT("Alles failed to register a configured owner");
            owners.emplace(owner, OwnerRuntime{});
        }
        if (settings.withholdFake)
            coordinator.SetFakeBehavior({Interpreter::FakeMode::Withhold, 0, true});
        if (settings.external)
            bridge = std::make_unique<Bridge::Service>(coordinator, settings.bridge);
        if (!settings.telemetryDirectory.empty())
        {
            // The directory outlives runs: file the previous run's journals away so this run starts clean.
            auto const archived = ::Alles::Telemetry::ArchivePreviousRun(settings.telemetryDirectory,
                ::Alles::Telemetry::PreviousRunLabel(settings.telemetryDirectory));
            boost::json::object manifest{{"schema", 1}, {"run", telemetryRun}, {"source", "alles-live"},
                {"label", "Ordinary realm — Alles pilot"}, {"timeBasis", "real elapsed milliseconds"},
                {"model", settings.external ? settings.bridge.model : "inprocess-fake"},
                {"profile", settings.bridge.profile}, {"journal", true}};
            recorder = std::make_unique<TelemetryRecorder>(settings.telemetryDirectory, telemetryRun,
                settings.owners, settings.telemetrySegmentBytes, startedRealMs, boost::json::serialize(manifest));
            ::Alles::Telemetry::InstallLiveSink(recorder.get());
            LOG_INFO("module.alles", "Alles telemetry journals in {} ({} previous-run files archived)",
                settings.telemetryDirectory, archived);
        }
        if (bridge && settings.conversation)
            conversation = std::make_unique<ConversationRuntime>(store, *bridge, recorder.get());
        if (settings.objectives || conversation)
            objectives = std::make_unique<ObjectiveRuntime>(store, settings.owners, recorder.get(),
                conversation.get(), bridge.get(), settings.objectives);
    }

    ~Impl()
    {
        ::Alles::Telemetry::InstallLiveSink(nullptr);
    }

    void CheckThread() const
    {
        if (thread != std::this_thread::get_id())
            ABORT("Alles runtime mutation requires the owning world thread");
    }

    bool Queue(IngressEvent event)
    {
        bool const critical = event.kind != IngressKind::Observe;
        if (ingress.Push(std::move(event)))
            return true;
        ++dropped;
        if (critical && !lifecycleFault.exchange(true))
            LOG_ERROR("module.alles", "Lifecycle ingress overflow; interpretation and speech are suspended");
        return false;
    }

    bool QueueCapture(Player const& observer, CaptureResult result)
    {
        if (!result.value)
        {
            if (result.status != CaptureStatus::SelfFeedback)
                ++dropped;
            return false;
        }
        return Queue({IngressKind::Observe, Owner(observer), Token(observer), std::move(*result.value)});
    }

    void DrainIngress(uint64_t gameMs, uint64_t realMs, std::size_t budget)
    {
        for (auto& event : ingress.Drain(budget))
        {
            if (!settings.owners.contains(event.owner))
                continue;
            auto& runtime = owners.at(event.owner);
            switch (event.kind)
            {
                case IngressKind::Activate:
                    store.Activate(event.owner, event.attachment);
                    coordinator.Track(event.owner);
                    break;
                case IngressKind::Save:
                    store.RequestFlush(event.owner);
                    break;
                case IngressKind::Logout:
                    store.Close(event.owner, event.attachment);
                    break;
                case IngressKind::Observe:
                {
                    if (!store.Status(event.owner))
                    {
                        store.Activate(event.owner, 0);
                        coordinator.Track(event.owner);
                    }
                    // Loading ingress is provisional. Only an admitted, still-recent audible query can affect speech.
                    bool const isQuestion = store.FindReady(event.owner)
                        && event.perception.kind == PerceptionKind::Speech && event.perception.comprehended
                        && event.perception.text.find('?') != std::string::npos
                        && gameMs >= event.perception.gameTimeMs && gameMs - event.perception.gameTimeMs <= 30000;
                    auto question = isQuestion ? event.perception.text : std::string{};
                    auto const questionTimeMs = event.perception.gameTimeMs;
                    std::optional<Perception> observed;
                    if (recorder)
                        observed = event.perception;
                    bool const admitted = store.Observe(event.owner, std::move(event.perception), realMs);
                    if (!admitted)
                        ++dropped;
                    else if (isQuestion)
                    {
                        runtime.question = std::move(question);
                        runtime.questionTimeMs = questionTimeMs;
                    }
                    if (observed)
                        recorder->RecordPerception(event.owner, *observed, admitted, realMs);
                    break;
                }
            }
        }
        for (auto& [owner, runtime] : owners)
            if (!runtime.question.empty() && (gameMs < runtime.questionTimeMs
                || gameMs - runtime.questionTimeMs > 30000))
                runtime.question.clear();
    }

    void ApplyCompletion(Storage::Completion const& completion, uint64_t gameMs, uint64_t realMs)
    {
        if (auto const* loaded = std::get_if<Storage::LoadResult>(&completion))
        {
            auto& runtime = owners.at(loaded->owner);
            runtime.loadInFlight = false;
            if (loaded->snapshot
                && store.FinishLoad(loaded->owner, loaded->generation, *loaded->snapshot, gameMs, realMs))
            {
                runtime.retryLoadMs = 0;
                return;
            }
            runtime.retryLoadMs = realMs + 5000;
            LOG_ERROR("module.alles", "Owner {}:{} load failed (outcome {}); retained state will not be overwritten",
                uint8_t(loaded->owner.kind), loaded->owner.id, uint32_t(loaded->outcome));
            return;
        }
        auto const& saved = std::get<Storage::SaveResult>(completion);
        if (saved.outcome == Storage::SaveOutcome::UndrainedOlderWrite)
            return; // A live future is not a failed transaction; preserve the store's in-flight fence.
        bool const committed = saved.outcome == Storage::SaveOutcome::Committed;
        store.CompleteSave(saved.owner, saved.generation, saved.revision, committed, realMs);
        if (!committed || saved.budgetExceeded)
            LOG_ERROR("module.alles", "Owner {}:{} revision {} save outcome {}, budget exceeded={}",
                uint8_t(saved.owner.kind), saved.owner.id, saved.revision, uint32_t(saved.outcome),
                saved.budgetExceeded);
    }

    void Poll(uint64_t gameMs, uint64_t realMs)
    {
        for (auto const& completion : dao.Poll(settings.itemBudget))
            ApplyCompletion(completion, gameMs, realMs);
    }

    void StartLoads(uint64_t realMs)
    {
        for (auto& [owner, runtime] : owners)
        {
            auto const status = store.Status(owner);
            if (status && status->state == ActorState::Loading && !runtime.loadInFlight
                && realMs >= runtime.retryLoadMs)
                runtime.loadInFlight = dao.StartLoad(owner, status->generation);
        }
    }

    void StartSaves(uint64_t realMs)
    {
        for (auto const& request : store.CaptureDueSaves(realMs, settings.itemBudget))
            if (!dao.StartSave(request))
                store.CompleteSave(request.snapshot.owner, request.generation, request.snapshot.revision,
                    false, realMs);
    }

    void Meetings(uint64_t gameMs, uint64_t realMs)
    {
        for (auto& [owner, runtime] : owners)
        {
            auto* observer = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32(owner.id)));
            if (!observer || !observer->IsInWorld())
            {
                runtime.visible.clear();
                continue;
            }
            auto const* visible = observer->GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
            if (!visible)
                continue;
            std::erase_if(runtime.metCooldown, [&](auto const& entry)
            {
                return gameMs >= entry.second && gameMs - entry.second >= 60000;
            });
            std::set<ObjectGuid> current;
            std::size_t examined = 0;
            for (auto const& [guid, object] : *visible)
            {
                if (++examined > 256)
                {
                    ++dropped;
                    break;
                }
                if (!object || !object->IsUnit())
                    continue;
                current.insert(guid);
                if (runtime.visible.contains(guid) || runtime.metCooldown.contains(guid))
                    continue;
                if (runtime.metCooldown.size() >= 256)
                {
                    ++dropped;
                    continue;
                }
                if (QueueCapture(*observer, CaptureMeeting(*observer, *object->ToUnit(), gameMs, realMs)))
                    runtime.metCooldown.emplace(guid, gameMs);
            }
            runtime.visible = std::move(current);
        }
    }

    bool HasAudience(Player const& speaker) const
    {
        auto const range = sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY);
        // Audience is live visibility, independent of the memory-owner allowlist.
        for (auto const& [guid, listener] : speaker.GetObjectVisibilityContainer().GetVisiblePlayersMap())
        {
            if (listener && listener != &speaker && listener->IsInWorld() && listener->IsInMap(&speaker)
                && listener->InSamePhase(&speaker) && listener->HaveAtClient(&speaker)
                && listener->CanSeeOrDetect(&speaker)
                && listener->GetExactDist(&speaker) <= range)
                return true;
        }
        return false;
    }

    void Speech(uint64_t gameMs, uint64_t realMs)
    {
        if (!settings.speech)
            return;
        std::size_t attempts = 0;
        for (auto& [owner, runtime] : owners)
        {
            if (attempts == 2)
                break;
            auto const status = store.Status(owner);
            if (!status || status->state != ActorState::Ready || !status->attachment
                || (runtime.lastSpeechMs && (gameMs < runtime.lastSpeechMs
                    || gameMs - runtime.lastSpeechMs < settings.speechCooldownMs)))
                continue;
            auto* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32(owner.id)));
            if (!player || Token(*player) != status->attachment || !EligibleSpeaker(*player) || !HasAudience(*player))
                continue;
            auto const* snapshot = store.FindReady(owner);
            Memory const* selected = nullptr;
            bool selectedRelevant = false;
            std::erase_if(runtime.spoken, [&](auto const& receipt)
            {
                return gameMs >= receipt.second && gameMs - receipt.second >= settings.speechRepeatMs;
            });
            bool const questionActive = !runtime.question.empty() && gameMs >= runtime.questionTimeMs
                && gameMs - runtime.questionTimeMs <= 30000;
            for (auto const& memory : snapshot->memories)
            {
                if ((memory.kind != MemoryKind::WitnessedDeath && memory.kind != MemoryKind::HeardStatement)
                    || memory.salience < settings.memory.provenanceFloor)
                    continue;
                auto const text = RenderMemory(memory);
                // Local player chat uses a 255-byte input limit. Never cut a quote/name halfway through a claim.
                if (text.empty() || text.size() > 255 || std::any_of(runtime.spoken.begin(), runtime.spoken.end(),
                    [&](auto const& receipt) { return receipt.first == text; }))
                    continue;
                bool const relevant = questionActive && !memory.subject.name.empty()
                    && runtime.question.find(memory.subject.name) != std::string::npos;
                if (!selected || (relevant && !selectedRelevant)
                    || (relevant == selectedRelevant && memory.salience > selected->salience))
                {
                    selected = &memory;
                    selectedRelevant = relevant;
                }
            }
            if (!selected)
                continue;
            auto const memoryId = selected->id;
            auto const line = RenderMemory(*selected);
            runtime.lastSpeechMs = gameMs;
            ++attempts;
            speakingOwner = owner;
            speakingText = line;
            speechDelivered = false;
            player->Say(line, LANG_UNIVERSAL);
            speakingOwner.reset();
            speakingText.clear();
            if (recorder)
                recorder->RecordSpeech(owner, line, speechDelivered, realMs);
            if (!speechDelivered)
                continue;
            store.Rehearse(owner, memoryId, gameMs, realMs, 0.05);
            runtime.spoken.emplace_back(line, gameMs);
            if (runtime.spoken.size() > 32)
                runtime.spoken.pop_front();
            runtime.question.clear();
        }
    }

    void Telemetry(uint64_t gameMs, uint64_t realMs)
    {
        if (!recorder || realMs < nextTelemetryMs)
            return;
        nextTelemetryMs = realMs + 1000;
        boost::json::array bots;
        std::set<uint32_t> online;
        for (auto const owner : settings.owners)
        {
            auto* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32(owner.id)));
            if (!p || !p->IsInWorld() || !p->GetSession() || !p->GetSession()->IsBot())
                continue;
            online.insert(uint32_t(owner.id));
            boost::json::array gear, bags, quests;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                auto const* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                gear.emplace_back(item ? item->GetEntry() : 0);
            }
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            {
                auto const* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                bags.emplace_back(boost::json::object{{"slot", slot}, {"item", item ? item->GetEntry() : 0},
                    {"capacity", item && item->IsBag() ? static_cast<Bag const*>(item)->GetBagSize() : 0}});
            }
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                auto id = p->GetQuestSlotQuestId(slot);
                if (!id) continue;
                boost::json::array objectives;
                for (uint8 objective = 0; objective < 4; ++objective)
                    objectives.emplace_back(p->GetQuestSlotCounter(slot, objective));
                quests.emplace_back(boost::json::object{{"id", id}, {"state", p->GetQuestSlotState(slot)},
                    {"objectives", objectives}});
            }
            auto const* memory = store.FindReady(owner);
            auto const status = store.Status(owner);
            boost::json::object bot{{"id", p->GetGUID().ToString()}, {"guid", owner.id},
                {"name", p->GetName()}, {"map", p->GetMapId()}, {"instance", p->GetInstanceId()},
                {"zone", p->GetZoneId()}, {"x", p->GetPositionX()}, {"y", p->GetPositionY()},
                {"z", p->GetPositionZ()}, {"level", p->GetLevel()}, {"xp", p->GetUInt32Value(PLAYER_XP)},
                {"nextLevelXp", p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)}, {"money", p->GetMoney()},
                {"health", p->GetHealth()}, {"maxHealth", p->GetMaxHealth()}, {"alive", p->IsAlive()},
                {"combat", p->IsInCombat()}, {"activity", !p->IsAlive() ? "dead" : p->IsInCombat() ? "combat"
                    : p->IsNonMeleeSpellCast(false) ? "casting" : p->isMoving() ? "moving" : "idle"},
                {"gear", gear}, {"bags", bags}, {"quests", quests}, {"deaths", nullptr},
                {"memoryCount", memory ? memory->memories.size() : 0},
                {"pendingPerceptions", memory ? memory->perceptions.size() : 0},
                {"memoryState", status ? ::Alles::Telemetry::StateName(status->state) : "unloaded"}};
            if (objectives)
                bot["planning"] = objectives->Status(owner);
            if (status)
            {
                bot["memoryRevision"] = status->revision;
                bot["committedRevision"] = status->committedRevision;
                bot["saving"] = status->saving;
                bot["saveFailed"] = status->saveFailed;
                bot["droppedPerceptions"] = status->droppedPerceptions;
            }
            if (recorder)
            {
                recorder->RecordOwner(owner, status, memory, realMs);
                if (auto const counters = recorder->Counters(owner))
                {
                    // Same meaning as the Observatory fields: counted from the core's event tap, not inferred.
                    bot["deaths"] = counters->deaths;
                    bot["earnedXp"] = counters->xp;
                    bot["questCompletions"] = counters->quests;
                    bot["aiUpdates"] = counters->aiUpdates;
                    bot["lastAiMs"] = counters->aiUpdates ? boost::json::value(recorder->SimMs(counters->lastAiMs))
                        : boost::json::value(nullptr);
                    bot["actions"] = counters->actions;
                    bot["lastAction"] = counters->lastAction;
                    bot["lastActionMs"] = counters->actions ? boost::json::value(recorder->SimMs(counters->lastActionMs))
                        : boost::json::value(nullptr);
                }
            }
            bots.emplace_back(std::move(bot));
        }
        auto elapsed = realMs - startedRealMs;
        auto interpreter = bridge ? bridge->Status() : boost::json::object{{"connected", false},
            {"mode", "inprocess-fake"}, {"model", "inprocess-fake"}, {"usedRequests", 0}, {"maxRequests", 0}};
        auto const& stats = coordinator.Stats();
        interpreter["stats"] = boost::json::object{{"dispatched", stats.dispatched},
            {"modelMemories", stats.modelMemories}, {"fakeMemories", stats.fakeMemories},
            {"fakeReinforcements", stats.fakeReinforcements}, {"fakeSupersessions", stats.fakeSupersessions},
            {"fallbackMemories", stats.fallbackMemories}, {"reflexMemories", stats.reflexMemories},
            {"invalidResults", stats.invalidResults}, {"invalidatedJobs", stats.invalidatedJobs},
            {"staleResults", stats.staleResults}, {"expiredJobs", stats.expiredJobs},
            {"contextOverflows", stats.contextOverflows}, {"resultDrops", stats.resultDrops}};
        boost::json::object snapshot{{"schema", 1}, {"source", "alles-live"}, {"run", telemetryRun},
            {"seq", ++telemetrySeq}, {"simMs", elapsed}, {"realMs", elapsed}, {"publishedUnixMs", gameMs},
            {"readOnly", true}, {"ready", true}, {"paused", false}, {"completed", false},
            {"requestedSpeed", 1}, {"achievedSpeed", 1.0}, {"backlogMs", 0}, {"maxTickUs", nullptr},
            {"expectedBots", bots.size()}, {"onlineBots", bots.size()},
            {"activeBots", recorder ? boost::json::value(recorder->ActiveBots(online, realMs))
                                   : boost::json::value(nullptr)},
            {"bots", bots}, {"interpreter", interpreter},
            {"alles", boost::json::object{{"owners", settings.owners.size()}, {"dropped", dropped.load()},
                {"unsafePackets", unsafePackets.load()}, {"lifecycleFault", lifecycleFault.load()}}}};
        if (conversation)
            snapshot["conversation"] = conversation->Status();
        if (recorder)
        {
            auto const totals = recorder->Totals();
            snapshot["runTotals"] = boost::json::object{{"xp", totals.xp}, {"quests", totals.quests},
                {"deaths", totals.deaths}};
            snapshot["journal"] = recorder->Status();
            bool connected = false;
            uint64_t used = 0;
            if (auto const* value = interpreter.if_contains("connected"); value && value->is_bool())
                connected = value->as_bool();
            if (auto const* value = interpreter.if_contains("usedRequests"); value && value->is_number())
                used = value->to_number<uint64_t>();
            recorder->RecordInterpreter(connected, used, realMs);
        }
        recorder->RecordSnapshot(boost::json::serialize(snapshot));
    }

    RuntimeSettings const settings;
    Ingress ingress;
    ActorStore store;
    Storage::SnapshotDao dao;
    Interpreter::PilotCoordinator coordinator;
    std::unique_ptr<Bridge::Service> bridge;
    std::unique_ptr<TelemetryRecorder> recorder;
    std::unique_ptr<ConversationRuntime> conversation;
    std::unique_ptr<ObjectiveRuntime> objectives;
    std::thread::id const thread;
    std::map<ActorKey, OwnerRuntime> owners;
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> unsafePackets{0};
    std::atomic<bool> lifecycleFault{false};
    std::atomic<bool> stopping{false};
    uint64_t nextDiscoveryMs = 0;
    uint64_t nextTelemetryMs = 0;
    uint64_t telemetrySeq = 0;
    uint64_t startedRealMs = RealNow();
    std::string telemetryRun = "alles-" + std::to_string(GameNow());
    std::optional<ActorKey> speakingOwner;
    std::string speakingText;
    bool speechDelivered = false;
};

Runtime::Runtime(RuntimeSettings settings) : _impl(std::make_unique<Impl>(std::move(settings))) { }
Runtime::~Runtime() = default;

bool Runtime::IsMainThread() const { return _impl->thread == std::this_thread::get_id(); }
bool Runtime::Contains(ActorKey owner) const { return _impl->settings.owners.contains(owner); }
uint64_t Runtime::Dropped() const { return _impl->dropped.load(); }
uint64_t Runtime::UnsafePackets() const { return _impl->unsafePackets.load(); }

void Runtime::Login(Player& player)
{
    if (_impl->conversation && IsMainThread())
        _impl->conversation->Login(player);
    if (!Contains(Owner(player)))
        return;
    _impl->CheckThread();
    auto const token = NextAttachment.fetch_add(1);
    if (!token || token == std::numeric_limits<uint64_t>::max())
        ABORT("Alles attachment token space exhausted");
    // The value lives in this Player incarnation. Queued logout/save records copy it before that body is deleted.
    player.CustomData.Set(AttachmentKey, new Attachment(token));
    _impl->Queue({IngressKind::Activate, Owner(player), token, {}});
}

void Runtime::Lifecycle(Player& player, IngressKind kind)
{
    if (kind == IngressKind::Logout && _impl->objectives && IsMainThread())
    {
        _impl->objectives->Detach(Owner(player), GameNow(), RealNow());
        _impl->objectives->RequesterLeft(Owner(player), RealNow());
    }
    if (kind == IngressKind::Logout && _impl->conversation && IsMainThread())
        _impl->conversation->Logout(player);
    if (!Contains(Owner(player)) || (kind != IngressKind::Save && kind != IngressKind::Logout))
        return;
    auto const token = Token(player);
    if (token)
        _impl->Queue({kind, Owner(player), token, {}});
}

void Runtime::Packet(Player& receiver, WorldPacket const& packet)
{
    if (packet.GetOpcode() == SMSG_EMOTE || packet.GetOpcode() == SMSG_LIST_INVENTORY)
    {
        if (IsMainThread() && !_impl->stopping && _impl->objectives)
            _impl->objectives->RequestPacket(receiver, packet);
        return;
    }
    if (packet.GetOpcode() != SMSG_MESSAGECHAT && packet.GetOpcode() != SMSG_GM_MESSAGECHAT
        && packet.GetOpcode() != SMSG_TEXT_EMOTE)
        return;
    // SendPacket also runs on map/socket paths. Without a proven delivery scope, do not touch receiver state.
    if (!IsMainThread())
    {
        ++_impl->unsafePackets;
        return;
    }
    bool const managed = Contains(Owner(receiver));
    if (_impl->stopping || (!managed && !NormalChatDeliveryPending()
        && (!_impl->speakingOwner || _impl->speechDelivered)))
        return;
    auto decoded = DecodeLocalPacket(packet);
    if (!decoded.value)
    {
        if (decoded.status != PacketDecodeStatus::Unsupported)
            ++_impl->dropped;
        return;
    }
    if (!decoded.value->source.IsPlayer())
    {
        // Nearby NPCs can send zone/map announcements with local-looking chat types. W7 must supply
        // emission-scope metadata before the runtime can admit their speech safely.
        ++_impl->dropped;
        return;
    }
    auto captured = CaptureDeliveredPacket(receiver, *decoded.value, GameNow(), RealNow());
    ObserveNormalChatDelivery(receiver, captured);
    if (managed && _impl->conversation && captured.value && captured.route)
        if (auto* source = ObjectAccessor::FindConnectedPlayer(decoded.value->source))
            _impl->conversation->Heard(receiver, *source, *captured.value, *captured.route);
    bool const delivered = _impl->speakingOwner && *_impl->speakingOwner != Owner(receiver)
        && captured.value && captured.value->source.actor == _impl->speakingOwner
        && captured.value->text == _impl->speakingText;
    if (!managed)
    {
        // Hearing a bot does not enroll a human in memory storage. Successful human delivery still
        // acknowledges speech so cooldown/deduplication and the speaker's rehearsal behave normally.
        if (delivered)
            _impl->speechDelivered = true;
        return;
    }
    if (_impl->QueueCapture(receiver, std::move(captured)) && delivered)
        _impl->speechDelivered = true;
}

void Runtime::OwnDeath(Player& player, Unit* killer)
{
    if (Contains(Owner(player)))
        _impl->QueueCapture(player, CaptureOwnDeath(player, GameNow(), RealNow(), killer));
}

void Runtime::WitnessDeath(Unit& victim, Unit* killer)
{
    // UnitScript's death callback is in the victim's safe map context. Only its already-visible player set is read.
    auto const gameMs = GameNow();
    auto const realMs = RealNow();
    std::size_t candidates = 0;
    for (auto const& [guid, observer] : victim.GetObjectVisibilityContainer().GetVisiblePlayersMap())
    {
        if (++candidates > 256)
        {
            ++_impl->dropped;
            break;
        }
        if (observer && observer->IsInMap(&victim) && Contains(Owner(*observer)))
            _impl->QueueCapture(*observer,
                CaptureWitnessedDeath(*observer, victim, killer, _impl->settings.witnessRange, gameMs, realMs));
    }
}

void Runtime::Update()
{
    _impl->CheckThread();
    auto const gameMs = GameNow();
    auto const realMs = RealNow();
    _impl->Poll(gameMs, realMs);
    if (_impl->recorder)
        _impl->recorder->Drain();
    _impl->DrainIngress(gameMs, realMs, _impl->settings.itemBudget);
    if (_impl->stopping)
        return;
    if (realMs >= _impl->nextDiscoveryMs)
    {
        _impl->nextDiscoveryMs = realMs + 1000;
        for (auto const owner : _impl->settings.owners)
        {
            auto* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32(owner.id)));
            if (player && !Token(*player))
                Login(*player);
        }
        if (!_impl->lifecycleFault)
            _impl->Meetings(gameMs, realMs);
    }
    _impl->StartLoads(realMs);
    if (!_impl->lifecycleFault)
    {
        _impl->store.Decay(gameMs, realMs);
        _impl->coordinator.Update(gameMs, realMs, _impl->settings.itemBudget);
        if (_impl->bridge)
            _impl->bridge->Update(gameMs, realMs);
        if (_impl->conversation)
            _impl->conversation->Update(gameMs, realMs);
        if (_impl->objectives)
            _impl->objectives->Update(gameMs, realMs);
        _impl->Speech(gameMs, realMs);
    }
    _impl->Telemetry(gameMs, realMs);
    _impl->StartSaves(realMs);
    for (auto const owner : _impl->settings.owners)
        // Management outlives the cached snapshot; eviction must not reset the owner's dispatch rate limit.
        _impl->store.EvictClosed(owner);
}

void Runtime::BeginShutdown()
{
    _impl->CheckThread();
    _impl->stopping = true;
    if (_impl->conversation)
        _impl->conversation->Stop();
    if (_impl->objectives)
        _impl->objectives->Stop(GameNow(), RealNow());
    _impl->coordinator.Stop();
}

void Runtime::FinishShutdown()
{
    _impl->CheckThread();
    // Maps are unloaded: no bot can raise another event, so the tap can be cleared without a race.
    Telemetry::InstallLiveSink(nullptr);
    BeginShutdown();
    auto const deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(_impl->settings.shutdownBudgetMs);
    auto const gameMs = GameNow();
    auto const realMs = RealNow();
    _impl->DrainIngress(gameMs, realMs, _impl->settings.ingressCapacity);
    _impl->dao.StopAsync();
    _impl->Poll(gameMs, realMs);
    // No Player/Creature/Unit lookup from here: maps and sessions have already unloaded.
    if (!_impl->lifecycleFault)
        while (std::chrono::steady_clock::now() < deadline
            && _impl->coordinator.DrainFallback(gameMs, RealNow(), _impl->settings.itemBudget)) { }
    for (auto const& completion : _impl->dao.DrainOlderWrites(deadline))
        _impl->ApplyCompletion(completion, gameMs, RealNow());
    for (auto const owner : _impl->settings.owners)
    {
        auto const status = _impl->store.Status(owner);
        if (!status)
            continue;
        if (status->state == ActorState::Loading || status->saving)
        {
            LOG_ERROR("module.alles", "Owner {}:{} remains undrained at shutdown", uint8_t(owner.kind), owner.id);
            continue;
        }
        if (auto const request = _impl->store.CaptureFinalSave(owner, RealNow()))
            _impl->ApplyCompletion(_impl->dao.CommitFinal(*request, deadline), gameMs, RealNow());
    }
    if (_impl->recorder)
    {
        _impl->recorder->Drain(_impl->settings.ingressCapacity);
        _impl->recorder->Flush();
    }
}

std::vector<std::string> Runtime::Recall(Player const& player, std::size_t count) const
{
    _impl->CheckThread();
    std::vector<std::string> memories;
    auto const status = _impl->store.Status(Owner(player));
    if (!status || status->attachment != Token(player) || status->state != ActorState::Ready)
        return memories;
    auto const* snapshot = _impl->store.FindReady(Owner(player));
    std::vector<Memory const*> ranked;
    for (auto const& memory : snapshot->memories)
        ranked.push_back(&memory);
    std::stable_sort(ranked.begin(), ranked.end(), [](Memory const* left, Memory const* right)
    {
        return left->salience > right->salience;
    });
    for (auto const* memory : ranked)
    {
        if (memories.size() == std::min<std::size_t>(count, 20))
            break;
        memories.push_back(RenderMemory(*memory));
    }
    return memories;
}

std::optional<OwnerStatus> Runtime::Status(ActorKey owner) const
{
    _impl->CheckThread();
    return Contains(owner) ? _impl->store.Status(owner) : std::nullopt;
}

std::optional<uint64_t> Runtime::Flush(ActorKey owner)
{
    _impl->CheckThread();
    if (!Contains(owner) || _impl->stopping)
        return std::nullopt;
    _impl->DrainIngress(GameNow(), RealNow(), _impl->settings.ingressCapacity);
    auto const status = _impl->store.Status(owner);
    if (!status || status->state == ActorState::Loading)
        return std::nullopt;
    _impl->store.RequestFlush(owner);
    return status->revision;
}

Runtime* ActiveRuntime() { return PublishedRuntime.load(std::memory_order_acquire); }
void PublishRuntime(Runtime* runtime) { PublishedRuntime.store(runtime, std::memory_order_release); }
}
