/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "ConversationRuntime.h"
#include "ConversationRouting.h"
#include "AttackAction.h"
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "StringFormat.h"
#include "telemetry/Recorder.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <deque>
#include <map>
#include <set>

namespace Alles
{

namespace
{
uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Player* Find(uint64_t guid)
{
    return ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, uint32_t(guid)));
}

bool Autonomous(Player& player)
{
    auto* ai = sPlayerbotsMgr.GetPlayerbotAI(&player);
    return player.IsInWorld() && player.IsAlive() && !player.IsBeingTeleported() && player.GetSession() &&
           player.GetSession()->IsBot() && !player.GetSession()->isLogingOut() && ai && !IsSelfBot(&player) &&
           !ai->IsExternallyControlled() && !ai->GetMaster() && !player.GetGroup();
}

bool Near(Player& bot, Player& human, uint8_t channel)
{
    float const range =
        sWorld->getFloatConfig(channel == CHAT_MSG_YELL ? CONFIG_LISTEN_RANGE_YELL : CONFIG_LISTEN_RANGE_SAY);
    return Autonomous(bot) && human.IsInWorld() && human.IsAlive() && !human.IsBeingTeleported() &&
           bot.IsInMap(&human) && bot.InSamePhase(&human) && bot.HaveAtClient(&human) && human.HaveAtClient(&bot) &&
           bot.CanSeeOrDetect(&human) && human.CanSeeOrDetect(&bot) && bot.GetExactDist(&human) <= range;
}

std::set<std::string> Words(std::string const& text)
{
    std::set<std::string> words;
    std::string word;
    for (unsigned char c : text + " ")
        if (std::isalnum(c))
            word += char(std::tolower(c));
        else
        {
            if (word.size() >= 4)
                words.insert(word);
            word.clear();
        }
    return words;
}

// Calls the existing attack action after the world has validated its exact target; no arbitrary chat command.
class AssistAction final : public AttackAction
{
public:
    explicit AssistAction(PlayerbotAI* ai) : AttackAction(ai, "alles assist") {}
    bool Start(Unit* target)
    {
        return Attack(target);
    }
};

Unit* Threat(Player& bot, Player& human)
{
    auto valid = [&](Unit* target)
    {
        return target && target->IsCreature() && !target->IsPet() && !target->GetCharmerOrOwnerGUID() &&
               target->IsAlive() && target->IsInMap(&bot) && target->InSamePhase(&bot) && bot.HaveAtClient(target) &&
               bot.CanSeeOrDetect(target) && bot.IsValidAttackTarget(target) && bot.GetExactDist(target) <= 40 &&
               bot.IsWithinLOSInMap(target) && (target->GetVictim() == &human || human.GetVictim() == target);
    };
    if (valid(human.GetVictim()))
        return human.GetVictim();
    for (auto* attacker : human.getAttackers())
        if (valid(attacker))
            return attacker;
    return nullptr;
}
} // namespace

struct ConversationRuntime::Impl
{
    struct Candidate
    {
        uint64_t bot = 0;
        uint64_t generation = 0;
        std::string name;
        std::string self;
        std::string place;
    };
    struct Turn
    {
        uint64_t human = 0;
        uint64_t generation = 0;
        uint64_t emitted = 0;
        uint64_t admitted = 0;
        uint8_t channel = CHAT_MSG_SAY;
        std::string text;
        std::vector<Candidate> candidates;
        std::vector<std::string> audience;
    };
    struct History
    {
        uint64_t last = 0;
        std::deque<boost::json::object> lines;
    };
    struct Pending
    {
        Turn turn;
        Candidate candidate;
        ObjectGuid threat;
    };
    struct Following
    {
        uint64_t human = 0;
        uint64_t humanGeneration = 0;
        uint64_t botGeneration = 0;
        uint64_t expires = 0;
        std::vector<std::string> strategies;
    };
    Impl(ActorStore& value, Bridge::Service& service, Telemetry::Recorder* telemetry)
        : store(value), bridge(service), recorder(telemetry) {}

    void Remember(uint64_t human, uint64_t bot, std::string const& who, std::string const& text, uint64_t now)
    {
        auto& history = histories[{human, bot}];
        history.last = now;
        history.lines.push_back({{"speaker", who}, {"text", text}});
        while (history.lines.size() > 8)
            history.lines.pop_front();
    }

    bool Valid(Pending const& pending)
    {
        auto* human = Find(pending.turn.human);
        auto* bot = Find(pending.candidate.bot);
        return human && bot && generations[pending.turn.human] == pending.turn.generation &&
               generations[pending.candidate.bot] == pending.candidate.generation &&
               Near(*bot, *human, pending.turn.channel);
    }

    void Release(uint64_t guid)
    {
        auto found = following.find(guid);
        if (found == following.end())
            return;
        auto* bot = Find(guid);
        if (bot && generations[guid] == found->second.botGeneration)
            if (auto* ai = sPlayerbotsMgr.GetPlayerbotAI(bot); ai && !IsSelfBot(bot) && !ai->IsExternallyControlled() &&
                                                               ai->GetStrategies(BOT_STATE_NON_COMBAT).empty())
            {
                ai->ClearStrategies(BOT_STATE_NON_COMBAT);
                for (auto const& name : found->second.strategies)
                    ai->ChangeStrategy("+" + name, BOT_STATE_NON_COMBAT);
                if (!bot->IsInCombat())
                {
                    bot->GetMotionMaster()->Clear();
                    bot->StopMoving();
                }
            }
        following.erase(found);
    }

    std::string Action(Pending const& pending, std::string const& action, uint64_t now)
    {
        auto& bot = *Find(pending.candidate.bot);
        auto& human = *Find(pending.turn.human);
        auto* ai = sPlayerbotsMgr.GetPlayerbotAI(&bot);
        if (action == "none")
            return {};
        if (!bot.IsFriendlyTo(&human))
            return "I can't help you with that.";
        if (action == "wave")
        {
            bot.HandleEmoteCommand(EMOTE_ONESHOT_WAVE);
            ++actions;
            return {};
        }
        if (action == "stop")
        {
            auto found = following.find(pending.candidate.bot);
            if (found == following.end() || found->second.human != pending.turn.human)
                return "I'm not following you at the moment.";
            Release(pending.candidate.bot);
            ++actions;
            return {};
        }
        if (action == "follow")
        {
            auto found = following.find(pending.candidate.bot);
            if (found != following.end() && found->second.human != pending.turn.human)
                return "I'm already accompanying someone else.";
            if (bot.IsInCombat() || bot.GetExactDist(&human) > 40 || !bot.IsWithinLOSInMap(&human))
                return "I can't start following you right now. Come closer when I'm out of combat.";
            if (found == following.end())
            {
                Following state{pending.turn.human, pending.turn.generation, pending.candidate.generation, now + 120000,
                                ai->GetStrategies(BOT_STATE_NON_COMBAT)};
                following.emplace(pending.candidate.bot, std::move(state));
                ai->ClearStrategies(BOT_STATE_NON_COMBAT);
            }
            else
                found->second.expires = now + 120000;
            bot.GetMotionMaster()->MoveFollow(&human, 3.0f, float(pending.candidate.bot % 6));
            ++actions;
            return {};
        }
        if (action == "assist")
        {
            auto* target = Threat(bot, human);
            if (!target || target->GetGUID() != pending.threat || !AssistAction(ai).Start(target))
                return "I can't engage that threat right now. Stay close and tell me what you need.";
            ++actions;
            return {};
        }
        return "I can't do that right now.";
    }

    boost::json::object Context(Turn const& turn, Candidate const& candidate, Player& bot, Player& human)
    {
        boost::json::array memories, history, audience;
        auto wanted = Words(turn.text);
        // Retrieve only matching evidence, with duplicate claims suppressed. Deaths are never a default topic.
        std::set<std::string> included;
        if (auto const* snapshot = store.FindReady({ActorKind::Player, candidate.bot}))
            for (auto const& memory : snapshot->memories)
            {
                auto text = RenderMemory(memory);
                if (text.size() > 512 || included.contains(text))
                    continue;
                auto words = Words(text);
                bool match =
                    std::any_of(wanted.begin(), wanted.end(), [&](auto const& word) { return words.contains(word); });
                if (match && memories.size() < 4)
                {
                    memories.emplace_back(text);
                    included.insert(text);
                }
            }
        auto found = histories.find({turn.human, candidate.bot});
        if (found != histories.end())
            for (auto const& line : found->second.lines)
                history.emplace_back(line);
        for (auto const& name : turn.audience)
            audience.emplace_back(name);
        auto follow = following.find(candidate.bot);
        auto* threat = Threat(bot, human);
        return {{"bot", candidate.name},
                {"selfContext", candidate.self},
                {"place", candidate.place},
                {"player", human.GetName()},
                {"message", turn.text},
                {"channel", turn.channel == CHAT_MSG_YELL ? "yell" : "say"},
                {"audience", audience},
                {"history", history},
                {"relevantMemories", memories},
                {"inCombat", bot.IsInCombat()},
                {"followingPlayer", follow != following.end() && follow->second.human == turn.human},
                {"canFollow", bot.IsFriendlyTo(&human) && !bot.IsInCombat() && bot.GetExactDist(&human) <= 40 &&
                                  (follow == following.end() || follow->second.human == turn.human)},
                {"nearbyThreat", threat ? threat->GetName() : ""},
                {"capabilities", "talk, wave, follow this player for up to two minutes, stop following, assist against "
                                 "the listed engaged NPC"}};
    }

    ActorStore& store;
    Bridge::Service& bridge;
    Telemetry::Recorder* const recorder;
    std::map<uint64_t, uint64_t> generations;
    uint64_t nextGeneration = 0;
    uint64_t nextJob = 0;
    std::deque<Turn> turns;
    std::map<std::string, Pending> pending;
    std::map<std::pair<uint64_t, uint64_t>, History> histories;
    std::map<uint64_t, Following> following;
    std::deque<Bridge::ConversationResult> results;
    uint64_t nextDelivery = 0;
    uint64_t nextMovement = 0;
    uint64_t replies = 0;
    uint64_t actions = 0;
    uint64_t omitted = 0;
};

ConversationRuntime::ConversationRuntime(ActorStore& store, Bridge::Service& bridge, Telemetry::Recorder* recorder)
    : impl(std::make_unique<Impl>(store, bridge, recorder))
{
}
ConversationRuntime::~ConversationRuntime() = default;

void ConversationRuntime::Login(Player& player)
{
    impl->generations[player.GetGUID().GetCounter()] = ++impl->nextGeneration;
}

void ConversationRuntime::Logout(Player& player)
{
    auto const guid = player.GetGUID().GetCounter();
    std::vector<uint64_t> release;
    for (auto const& [bot, state] : impl->following)
        if (bot == guid || state.human == guid)
            release.push_back(bot);
    for (auto bot : release)
        impl->Release(bot);
    impl->generations.erase(guid);
    std::erase_if(impl->histories,
                  [&](auto const& entry) { return entry.first.first == guid || entry.first.second == guid; });
}

void ConversationRuntime::Heard(Player& bot, Player& human, Perception const& perception, uint8_t channel)
{
    if (!perception.comprehended || perception.kind != PerceptionKind::Speech || !human.GetSession() ||
        human.GetSession()->IsBot() || IsSelfBot(&human) || !Near(bot, human, channel) ||
        (channel != CHAT_MSG_SAY && channel != CHAT_MSG_YELL) || perception.text.size() > 255)
        return;
    auto const humanId = human.GetGUID().GetCounter();
    auto const botId = bot.GetGUID().GetCounter();
    if (!impl->generations.contains(humanId))
        Login(human);
    if (!impl->generations.contains(botId))
        Login(bot);
    auto now = Now();
    auto turn = std::find_if(impl->turns.begin(), impl->turns.end(),
                             [&](auto const& item)
                             {
                                 return item.human == humanId && item.emitted == perception.gameTimeMs &&
                                        item.text == perception.text && item.channel == channel;
                             });
    if (turn == impl->turns.end())
    {
        if (impl->turns.size() >= 32 || std::count_if(impl->turns.begin(), impl->turns.end(),
                                                      [&](auto const& item) { return item.human == humanId; }) >= 3)
        {
            ++impl->omitted;
            return;
        }
        impl->turns.push_back(
            {humanId, impl->generations[humanId], perception.gameTimeMs, now, channel, perception.text});
        turn = std::prev(impl->turns.end());
    }
    if (std::none_of(turn->candidates.begin(), turn->candidates.end(),
                     [&](auto const& candidate) { return candidate.bot == botId; }))
    {
        turn->candidates.push_back(
            {botId, impl->generations[botId], bot.GetName(), perception.selfContext, perception.place});
        turn->audience.push_back(bot.GetName());
    }
}

void ConversationRuntime::Update(uint64_t gameMs, uint64_t realMs)
{
    std::erase_if(impl->histories, [&](auto const& entry) { return realMs - entry.second.last >= 120000; });
    for (auto it = impl->pending.begin(); it != impl->pending.end();)
        if (!impl->Valid(it->second))
        {
            impl->bridge.CancelConversation(it->first);
            it = impl->pending.erase(it);
            ++impl->omitted;
        }
        else
            ++it;
    for (auto& result : impl->bridge.TakeConversations())
        impl->results.push_back(std::move(result));
    if (!impl->results.empty() && realMs >= impl->nextDelivery)
    {
        auto result = std::move(impl->results.front());
        impl->results.pop_front();
        auto found = impl->pending.find(result.id);
        if (found != impl->pending.end())
        {
            auto const& pending = found->second;
            auto* bot = Find(pending.candidate.bot);
            auto* human = Find(pending.turn.human);
            if (bot && human && impl->Valid(pending))
            {
                if (result.status == "success" && result.response.at("reply").as_bool())
                {
                    auto text = Bridge::String(result.response, "text", 255);
                    auto action = Bridge::String(result.response, "action", 16);
                    auto failure = impl->Action(pending, action, realMs);
                    if (!failure.empty())
                        text = failure;
                    if (pending.turn.channel == CHAT_MSG_YELL)
                        bot->Yell(text, LANG_UNIVERSAL);
                    else
                        bot->Say(text, LANG_UNIVERSAL);
                    impl->Remember(pending.turn.human, pending.candidate.bot, bot->GetName(), text, realMs);
                    ++impl->replies;
                    impl->nextDelivery = realMs + 1000;
                    LOG_INFO("module.alles", "Conversation {} delivered bot={} human={} action={} actionOk={}",
                             result.id, pending.candidate.bot, pending.turn.human, action, failure.empty());
                    if (impl->recorder)
                        impl->recorder->Record({ActorKind::Player, pending.candidate.bot}, "alles_conversation",
                            failure.empty() ? 1 : 0, text,
                            Acore::StringFormat("human={} channel={} action={} actionOk={} job={}",
                                pending.turn.human, pending.turn.channel == CHAT_MSG_YELL ? "yell" : "say",
                                action, failure.empty(), result.id), realMs);
                }
                else if (result.status != "success")
                    ChatHandler(human->GetSession())
                        .PSendSysMessage("{} couldn't answer in time. Please try again.", bot->GetName());
            }
            impl->pending.erase(found);
        }
    }
    for (auto turn = impl->turns.begin(); turn != impl->turns.end();)
    {
        if (realMs - turn->admitted >= 30000 || impl->generations[turn->human] != turn->generation)
        {
            ++impl->omitted;
            turn = impl->turns.erase(turn);
            continue;
        }
        if (realMs - turn->admitted < 250)
        {
            ++turn;
            continue;
        }
        for (auto candidate = turn->candidates.begin(); candidate != turn->candidates.end();)
        {
            if (auto addressed = AddressedBot(turn->text, turn->audience); addressed && *addressed != candidate->name)
            {
                candidate = turn->candidates.erase(candidate);
                continue;
            }
            bool const busy = std::any_of(
                impl->pending.begin(), impl->pending.end(), [&](auto const& item)
                { return item.second.turn.human == turn->human && item.second.candidate.bot == candidate->bot; });
            if (busy || impl->pending.size() >= 32)
            {
                ++candidate;
                continue;
            }
            Impl::Pending pending{*turn, *candidate, {}};
            if (impl->Valid(pending))
            {
                auto& bot = *Find(candidate->bot);
                auto& human = *Find(turn->human);
                if (auto* threat = Threat(bot, human))
                    pending.threat = threat->GetGUID();
                auto context = impl->Context(*turn, *candidate, bot, human);
                auto id = "conversation-" + std::to_string(gameMs) + "-" + std::to_string(++impl->nextJob);
                if (!impl->bridge.QueueConversation(id, std::move(context), realMs))
                {
                    ++candidate;
                    continue;
                }
                impl->Remember(turn->human, candidate->bot, human.GetName(), turn->text, realMs);
                impl->pending.emplace(id, std::move(pending));
            }
            candidate = turn->candidates.erase(candidate);
        }
        if (turn->candidates.empty())
            turn = impl->turns.erase(turn);
        else
            ++turn;
    }
    if (realMs >= impl->nextMovement)
    {
        impl->nextMovement = realMs + 1000;
        std::vector<uint64_t> release;
        for (auto const& [guid, state] : impl->following)
        {
            auto* bot = Find(guid);
            auto* human = Find(state.human);
            if (!bot || !human || !Autonomous(*bot) || !human->IsAlive() || !human->IsInWorld() ||
                impl->generations[guid] != state.botGeneration ||
                impl->generations[state.human] != state.humanGeneration || realMs >= state.expires ||
                !bot->IsInMap(human) || !bot->InSamePhase(human) || bot->GetExactDist(human) > 100)
                release.push_back(guid);
            else if (!bot->IsInCombat() &&
                     bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                bot->GetMotionMaster()->MoveFollow(human, 3.0f, float(guid % 6));
        }
        for (auto guid : release)
            impl->Release(guid);
    }
}

void ConversationRuntime::Stop()
{
    while (!impl->following.empty())
        impl->Release(impl->following.begin()->first);
    for (auto const& [id, pending] : impl->pending)
        impl->bridge.CancelConversation(id);
    impl->pending.clear();
    impl->turns.clear();
}

boost::json::object ConversationRuntime::Status() const
{
    return {{"enabled", true},          {"queuedTurns", impl->turns.size()}, {"pendingReplies", impl->pending.size()},
            {"replies", impl->replies}, {"actions", impl->actions},          {"following", impl->following.size()},
            {"omitted", impl->omitted}};
}
} // namespace Alles
