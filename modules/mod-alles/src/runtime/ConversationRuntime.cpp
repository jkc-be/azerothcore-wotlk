/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "ConversationRuntime.h"
#include "PartyActions.h"
#include "AdvicePlanning.h"
#include "ObjectiveRuntime.h"
#include "ConversationRouting.h"
#include "ConversationPolicy.h"
#include "ConversationKnowledge.h"
#include "ChatSender.h"
#include "perception/ChatAudience.h"
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
#include <tuple>

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

bool CanConverse(Player& player)
{
    auto* ai = sPlayerbotsMgr.GetPlayerbotAI(&player);
    return player.IsInWorld() && player.IsAlive() && !player.IsBeingTeleported() && player.GetSession() &&
           player.GetSession()->IsBot() && !player.GetSession()->isLogingOut() && ai && !IsSelfBot(&player) &&
           !ai->IsExternallyControlled() && (CurrentControlMode(player) == ControlMode::AutonomousSolo
               || CurrentControlMode(player) == ControlMode::AutonomousParty);
}

bool Autonomous(Player& player)
{
    return CanConverse(player) && !player.GetGroup();
}

bool Near(Player& bot, Player& human, uint8_t channel)
{
    float const range =
        sWorld->getFloatConfig(channel == CHAT_MSG_YELL ? CONFIG_LISTEN_RANGE_YELL : CONFIG_LISTEN_RANGE_SAY);
    return CanConverse(bot) && human.IsInWorld() && human.IsAlive() && !human.IsBeingTeleported() &&
           bot.IsInMap(&human) && bot.InSamePhase(&human) && bot.HaveAtClient(&human) && human.HaveAtClient(&bot) &&
           bot.CanSeeOrDetect(&human) && human.CanSeeOrDetect(&bot) && bot.GetExactDist(&human) <= range;
}

std::string ConversationScope(Player const& speaker, Player const& receiver, SpeechRoute const& route)
{
    if (route.type == CHAT_MSG_WHISPER)
    {
        auto const first = std::min(speaker.GetGUID(), receiver.GetGUID());
        auto const second = std::max(speaker.GetGUID(), receiver.GetGUID());
        return "whisper:" + first.ToString() + ":" + second.ToString();
    }
    if (route.type == CHAT_MSG_PARTY)
        return "party:" + std::to_string(route.groupId) + ":" + std::to_string(route.subgroup);
    if (route.type == CHAT_MSG_CHANNEL)
        return "channel:" + std::to_string(route.channelId) + ":" + route.channelName;
    // Local speech has already passed Near. Remote audiences never read a speaker's position or phase.
    return "local:" + std::to_string(speaker.GetMapId()) + ":" + std::to_string(speaker.GetPhaseMask());
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
        uint64_t actorGeneration = 0;
    };
    struct Turn
    {
        uint64_t human = 0;
        uint64_t generation = 0;
        uint64_t emitted = 0;
        uint64_t admitted = 0;
        SpeechRoute route;
        std::string text;
        std::vector<Candidate> candidates;
        std::vector<std::string> audience;
        bool elected = false;
        uint64_t thread = 0;
        uint64_t dispatchAfter = 0;
        bool sourceBot = false;
        std::optional<RecruitmentNotice> recruitment;

        uint64_t Lifetime() const { return recruitment ? 120000 : 30000; }
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
    struct Question
    {
        InformationQuestion request;
        SpeechRoute route;
        uint64_t thread = 0;
        uint64_t expires = 0;
        std::vector<std::pair<Reference, std::string>> seenReplies;
    };
    struct RosterEmission
    {
        CooperativeRoster roster;
        ActorKey recipient;
        bool accepted = false;
    };
    Impl(ActorStore& value, Bridge::Service& service, Telemetry::Recorder* telemetry)
        : store(value), bridge(service), recorder(telemetry) {}

    using HistoryKey = ConversationHistoryKey;

    static HistoryKey Key(uint64_t human, uint64_t bot, SpeechRoute const& route)
    {
        // Both participants know this delivered dialogue even when the next reply reverses their roles.
        return SharedDialogueKey(human, bot, route);
    }

    void Remember(uint64_t human, uint64_t bot, SpeechRoute const& route,
        std::string const& who, std::string const& text, uint64_t now)
    {
        auto const key = Key(human, bot, route);
        if (!histories.contains(key) && histories.size() >= 128)
        {
            auto oldest = std::min_element(histories.begin(), histories.end(),
                [](auto const& left, auto const& right) { return left.second.last < right.second.last; });
            histories.erase(oldest);
        }
        auto& history = histories[key];
        history.last = now;
        history.lines.push_back({{"speaker", who}, {"text", text}});
        while (history.lines.size() > 8)
            history.lines.pop_front();
    }

    bool Valid(Pending const& pending)
    {
        auto* human = Find(pending.turn.human);
        auto* bot = Find(pending.candidate.bot);
        auto const* thread = policy.Find(pending.turn.thread);
        return thread && Now() < thread->expiresMs && human && bot &&
               generations[pending.turn.human] == pending.turn.generation &&
               generations[pending.candidate.bot] == pending.candidate.generation &&
               CanConverse(*bot) && Now() - pending.turn.admitted < pending.turn.Lifetime() &&
               CaptureSpeechRoute(*human, *bot, pending.turn.route.type, pending.turn.route.channelName)
                   == pending.turn.route &&
               (IsRemoteSpeech(pending.turn.route.type) || Near(*bot, *human, pending.turn.route.type));
    }

    std::string Action(Pending const& pending, std::string const& action, std::string const& statement,
        uint64_t gameMs, uint64_t now)
    {
        auto& bot = *Find(pending.candidate.bot);
        auto& human = *Find(pending.turn.human);
        if (action == "none")
            return {};
        if (action == "offer_help")
        {
            if (!objectives || !pending.turn.recruitment
                || !objectives->OfferHelp({ActorKind::Player, pending.candidate.bot},
                    pending.candidate.actorGeneration, *pending.turn.recruitment, statement, gameMs, now))
                return "I can't commit to joining that quest right now.";
            ++actions;
            return {};
        }
        if (!Autonomous(bot) || pending.turn.sourceBot || IsRemoteSpeech(pending.turn.route.type)
            || !Near(bot, human, pending.turn.route.type))
            return "I can only talk with you from here.";
        if (!objectives)
            return "I can't act on that request right now.";
        auto failure = objectives->ApplyHumanRequest({ActorKind::Player, pending.candidate.bot},
            pending.candidate.actorGeneration, {ActorKey{ActorKind::Player, pending.turn.human}, human.GetName()},
            pending.turn.text, action, pending.threat, gameMs, now);
        if (failure.empty())
            ++actions;
        return failure;
    }

    boost::json::object Context(Turn const& turn, Candidate const& candidate, Player& bot, Player& human)
    {
        boost::json::array history, audience;
        ConversationKnowledge evidence;
        if (auto const* snapshot = store.FindReady({ActorKind::Player, candidate.bot}))
            evidence = RetrieveConversationKnowledge(*snapshot, turn.text, uint8_t(bot.GetLevel()), turn.emitted);
        auto found = histories.find(Key(turn.human, candidate.bot, turn.route));
        if (found != histories.end())
            for (auto const& line : found->second.lines)
                history.emplace_back(line);
        for (auto const& name : turn.audience)
            audience.emplace_back(name);
        bool const local = Autonomous(bot) && !turn.sourceBot && !IsRemoteSpeech(turn.route.type)
            && Near(bot, human, turn.route.type);
        auto humanContext = objectives && local
            ? objectives->HumanContext({ActorKind::Player, candidate.bot}, candidate.actorGeneration,
                {ActorKind::Player, turn.human})
            : boost::json::object{{"localActions", false}, {"canFollow", false}, {"followingPlayer", false},
                {"nearbyThreat", ""}};
        auto const* thread = policy.Find(turn.thread);
        auto const remaining = thread ? (thread->recruitmentLimit ? thread->recruitmentLimit : 4)
            - thread->attempts : 0;
        boost::json::object context{{"bot", candidate.name},
                {"selfContext", candidate.self},
                {"place", candidate.place},
                {"player", human.GetName()},
                {"message", turn.text},
                {"gameTimeMs", turn.emitted},
                {"channel", SpeechRouteLabel(turn.route)},
                {"localActions", humanContext.at("localActions")},
                {"speakerIsBot", turn.sourceBot},
                {"threadRepliesRemaining", remaining},
                {"audience", audience},
                {"history", history},
                {"relevantMemories", evidence.memories},
                {"knownPlaces", evidence.places},
                {"learnedReports", evidence.reports},
                {"inCombat", bot.IsInCombat()},
                {"followingPlayer", humanContext.at("followingPlayer")},
                {"canFollow", humanContext.at("canFollow")},
                {"nearbyThreat", humanContext.at("nearbyThreat")},
                {"capabilities", humanContext.at("localActions").as_bool()
                    ? "talk, wave, follow this player for up to two minutes, stop following, "
                        "assist against the listed engaged NPC"
                    : "talk through the delivered channel"}};
        if (objectives && turn.recruitment)
        {
            auto offer = objectives->HelpOfferContext({ActorKind::Player, candidate.bot}, candidate.actorGeneration,
                *turn.recruitment, turn.emitted, Now());
            context["canOfferHelp"] = offer.at("canOfferHelp");
            context["recruitment"] = std::move(offer);
        }
        return context;
    }

    struct Emission
    {
        uint64_t thread = 0;
        uint64_t recipient = 0;
    };
    struct EmissionScope
    {
        std::optional<Emission>& slot;
        EmissionScope(std::optional<Emission>& value, uint64_t thread, uint64_t recipient) : slot(value)
        {
            slot = Emission{thread, recipient};
        }
        ~EmissionScope() { slot.reset(); }
    };

    ActorStore& store;
    Bridge::Service& bridge;
    Telemetry::Recorder* const recorder;
    ObjectiveRuntime* objectives = nullptr;
    std::optional<RosterEmission> rosterEmission;
    ConversationPolicy policy;
    std::map<ActorKey, Question> questions;
    std::deque<InformationReply> informationReplies;
    uint64_t questionsAttempted = 0;
    uint64_t questionsDelivered = 0;
    std::optional<Emission> emission;
    std::map<uint64_t, uint64_t> generations;
    uint64_t nextGeneration = 0;
    uint64_t nextJob = 0;
    std::deque<Turn> turns;
    std::map<std::string, Pending> pending;
    std::map<HistoryKey, History> histories;
    std::deque<Bridge::ConversationResult> results;
    uint64_t nextDelivery = 0;
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
    impl->generations.erase(guid);
    impl->policy.Forget(guid);
    ActorKey const owner{ActorKind::Player, guid};
    impl->questions.erase(owner);
    std::erase_if(impl->informationReplies, [owner](auto const& reply) { return reply.question.owner == owner; });
    std::erase_if(impl->histories,
                  [&](auto const& entry)
                  { return std::get<0>(entry.first) == guid || std::get<1>(entry.first) == guid; });
}

void ConversationRuntime::Heard(Player& bot, Player& human, Perception const& perception, SpeechRoute const& route)
{
    if (!perception.comprehended || perception.kind != PerceptionKind::Speech || !human.GetSession() ||
        IsSelfBot(&human) || !CanConverse(bot) ||
        (!IsRemoteSpeech(route.type) && !Near(bot, human, route.type)) || perception.text.size() > 255)
        return;
    auto const humanId = human.GetGUID().GetCounter();
    auto const botId = bot.GetGUID().GetCounter();
    if (impl->rosterEmission && impl->rosterEmission->recipient == ActorKey{ActorKind::Player, botId}
        && impl->rosterEmission->roster.leader == perception.source.actor
        && impl->rosterEmission->roster.text == perception.text && route.type == CHAT_MSG_WHISPER)
    {
        auto const status = impl->store.Status({ActorKind::Player, botId});
        impl->rosterEmission->accepted = impl->objectives && status
            && impl->objectives->ReceiveRoster({ActorKind::Player, botId}, status->generation,
                impl->rosterEmission->roster, perception.source, perception.gameTimeMs, Now());
        return;
    }
    // Everyone can retain actually heard speech, but an answer only offers its intended peer a reply turn.
    if (impl->emission && impl->emission->recipient && impl->emission->recipient != botId)
        return;
    if (!impl->generations.contains(humanId))
        Login(human);
    if (!impl->generations.contains(botId))
        Login(bot);
    auto now = Now();
    auto question = impl->questions.find({ActorKind::Player, botId});
    if (question != impl->questions.end() && now < question->second.expires
        && (route == question->second.route || route.type == CHAT_MSG_WHISPER)
        && impl->informationReplies.size() < 256)
    {
        auto const& request = question->second.request;
        auto& seen = question->second.seenReplies;
        bool const duplicate = std::any_of(seen.begin(), seen.end(), [&](auto const& reply)
            { return reply.first == perception.source && reply.second == perception.text; });
        if (seen.size() < 8 && !duplicate)
        {
            seen.emplace_back(perception.source, perception.text);
            impl->informationReplies.push_back({request, perception.source, perception.text,
                SpeechRouteLabel(route), perception.gameTimeMs, now});
        }
    }
    auto turn = std::find_if(impl->turns.begin(), impl->turns.end(),
                             [&](auto const& item)
                             {
                                 return item.human == humanId && item.emitted == perception.gameTimeMs &&
                                        item.text == perception.text && item.route == route;
                             });
    if (turn == impl->turns.end())
    {
        if (impl->turns.size() >= 32 || std::count_if(impl->turns.begin(), impl->turns.end(),
                                                      [&](auto const& item) { return item.human == humanId; }) >= 3)
        {
            ++impl->omitted;
            return;
        }
        auto thread = impl->emission ? std::optional<uint64_t>(impl->emission->thread)
            : impl->policy.Open(humanId, ConversationScope(human, bot, route), perception.text, now);
        if (!thread)
        {
            ++impl->omitted;
            return;
        }
        impl->turns.push_back(
            {humanId, impl->generations[humanId], perception.gameTimeMs, now, route, perception.text});
        turn = std::prev(impl->turns.end());
        turn->thread = *thread;
        turn->sourceBot = human.GetSession()->IsBot();
        auto recruitment = impl->questions.find({ActorKind::Player, humanId});
        if (recruitment != impl->questions.end() && now < recruitment->second.expires
            && recruitment->second.route == route && recruitment->second.request.text == perception.text
            && recruitment->second.request.topic.activity == Activity::Companions
            && perception.source.actor == recruitment->second.request.owner)
            turn->recruitment = RecruitmentNotice{recruitment->second.request, perception.source,
                perception.gameTimeMs};
    }
    if (turn->candidates.size() < 32 && std::none_of(turn->candidates.begin(), turn->candidates.end(),
                     [&](auto const& candidate) { return candidate.bot == botId; }))
    {
        auto const status = impl->store.Status({ActorKind::Player, botId});
        turn->candidates.push_back({botId, impl->generations[botId], bot.GetName(), perception.selfContext,
            perception.place, status ? status->generation : 0});
        turn->audience.push_back(bot.GetName());
    }
}

bool ConversationRuntime::CanAsk(ActorKey owner) const
{
    if (owner.kind != ActorKind::Player || !IsValidActor(owner) || impl->questions.contains(owner))
        return false;
    auto* bot = Find(owner.id);
    return bot && CanConverse(*bot) && !bot->IsInCombat() && QuestionRoute(*bot).has_value();
}

void ConversationRuntime::SetObjectives(ObjectiveRuntime* objectives)
{
    impl->objectives = objectives;
}

bool ConversationRuntime::RecruitmentActive(RecruitmentNotice const& notice, uint64_t realMs) const
{
    auto found = impl->questions.find(notice.question.owner);
    if (found == impl->questions.end() || realMs >= found->second.expires
        || notice.source.actor != notice.question.owner)
        return false;
    auto const& current = found->second.request;
    return current.topic.activity == Activity::Companions && current.generation == notice.question.generation
        && current.objective == notice.question.objective && current.attempt == notice.question.attempt
        && current.topic == notice.question.topic && current.text == notice.question.text
        && current.questName == notice.question.questName && current.placeName == notice.question.placeName
        && current.partySize == notice.question.partySize;
}

bool ConversationRuntime::SendRoster(ActorKey owner, ActorKey recipient, CooperativeRoster const& roster,
    uint64_t gameMs, uint64_t realMs)
{
    auto* sender = Find(owner.id);
    auto* receiver = Find(recipient.id);
    if (owner.kind != ActorKind::Player || recipient.kind != ActorKind::Player || owner == recipient
        || roster.leader != owner || !sender || !receiver || !CanConverse(*sender)
        || !receiver->GetSession() || !IsSafeChatText(roster.text) || impl->rosterEmission)
        return false;
    auto const route = CaptureSpeechRoute(*receiver, *sender, CHAT_MSG_WHISPER, {});
    if (!route)
        return false;
    auto thread = impl->policy.Open(owner.id, ConversationScope(*sender, *receiver, *route), roster.text, realMs);
    if (!thread || !impl->policy.ReserveQuestion(*thread, realMs))
        return false;
    impl->rosterEmission = Impl::RosterEmission{roster, recipient};
    struct ClearEmission
    {
        std::optional<Impl::RosterEmission>& emission;
        ~ClearEmission() { emission.reset(); }
    } clear{impl->rosterEmission};
    bool const delivered = SendNormalChat(*sender, *receiver, *route, roster.text);
    bool const adopted = !receiver->GetSession()->IsBot() || impl->rosterEmission->accepted;
    impl->rosterEmission.reset();
    if (impl->recorder)
        impl->recorder->Record(owner, "alles_roster", delivered && adopted ? 1 : 0, roster.text,
            Acore::StringFormat("recipient={} delivered={} adopted={} gameMs={}",
                recipient.id, delivered, adopted, gameMs), realMs);
    return delivered && adopted;
}

bool ConversationRuntime::Ask(InformationQuestion const& question, uint64_t gameMs, uint64_t realMs)
{
    if (!CanAsk(question.owner) || !IsSafeChatText(question.text))
        return false;
    auto const status = impl->store.Status(question.owner);
    auto const* snapshot = impl->store.FindReady(question.owner);
    if (!status || status->generation != question.generation || !snapshot || !snapshot->planning)
        return false;
    auto found = snapshot->planning->objectives.objectives.find(question.objective);
    if (found == snapshot->planning->objectives.objectives.end())
        return false;
    auto const& objective = found->second;
    PrivateKnowledge knowledge;
    if (question.topic.activity == Activity::Companions && !knowledge.Restore(snapshot->planning->knowledge))
        return false;
    if (objective.revision != question.revision || objective.information.status != InformationStatus::Pending
        || objective.information.attempts != question.attempt || objective.information.question != question.text
        || !((objective.quest == question.topic.quest && objective.place == question.topic.place
                && question.topic.activity == Activity::Work && !question.topic.person)
            || MatchesRecruitment(question, objective, knowledge, gameMs)))
        return false;
    auto* bot = Find(question.owner.id);
    auto route = QuestionRoute(*bot);
    if (!route)
        return false;
    auto thread = impl->policy.Open(question.owner.id, ConversationScope(*bot, *bot, *route), question.text, realMs);
    if (!thread || !impl->policy.ReserveQuestion(*thread, realMs))
        return false;
    if (question.topic.activity == Activity::Companions
        && !impl->policy.EnableRecruitment(*thread, question.partySize - 1))
        return false;
    impl->questions.emplace(question.owner, Impl::Question{question, *route, *thread, realMs + 120000});
    ++impl->questionsAttempted;
    bool delivered = false;
    {
        Impl::EmissionScope emission(impl->emission, *thread, 0);
        delivered = SendNormalQuestion(*bot, *route, question.text);
    }
    if (delivered)
        ++impl->questionsDelivered;
    else
        impl->questions.erase(question.owner);
    if (impl->recorder)
        impl->recorder->Record(question.owner, "alles_question", delivered ? 1 : 0, question.text,
            Acore::StringFormat("objective={} attempt={} thread={} channel={} gameMs={}",
                question.objective, question.attempt, *thread, SpeechRouteLabel(*route), gameMs), realMs);
    return delivered;
}

std::vector<InformationReply> ConversationRuntime::TakeInformationReplies(ActorKey owner)
{
    std::vector<InformationReply> replies;
    for (auto it = impl->informationReplies.begin(); it != impl->informationReplies.end();)
        if (it->question.owner == owner)
        {
            replies.push_back(std::move(*it));
            it = impl->informationReplies.erase(it);
        }
        else
            ++it;
    return replies;
}

void ConversationRuntime::Update(uint64_t gameMs, uint64_t realMs)
{
    impl->policy.Prune(realMs);
    std::erase_if(impl->questions, [&](auto const& entry)
    {
        auto const status = impl->store.Status(entry.first);
        auto* bot = Find(entry.first.id);
        return realMs >= entry.second.expires || !status || status->generation != entry.second.request.generation
            || !bot || !CanConverse(*bot);
    });
    std::erase_if(impl->informationReplies, [realMs](auto const& reply)
        { return realMs >= reply.realMs && realMs - reply.realMs >= 120000; });
    std::erase_if(impl->histories, [&](auto const& entry) { return realMs - entry.second.last >= 120000; });
    for (auto it = impl->pending.begin(); it != impl->pending.end();)
        if (!impl->Valid(it->second))
        {
            impl->bridge.CancelConversation(it->first);
            impl->policy.Finish(it->second.turn.thread);
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
                if (result.status == "success" && result.response.at("reply").as_bool()
                    && !impl->policy.DuplicateAnswer(pending.turn.thread,
                        Bridge::String(result.response, "text", 255), realMs))
                {
                    auto text = Bridge::String(result.response, "text", 255);
                    auto action = Bridge::String(result.response, "action", 16);
                    auto failure = impl->Action(pending, action, text, gameMs, realMs);
                    if (!failure.empty())
                        text = failure;
                    bool delivered = false;
                    {
                        Impl::EmissionScope emission(impl->emission, pending.turn.thread, pending.turn.human);
                        delivered = SendNormalChat(*bot, *human, pending.turn.route, text);
                    }
                    if (delivered)
                    {
                        impl->Remember(pending.turn.human, pending.candidate.bot, pending.turn.route,
                            bot->GetName(), text, realMs);
                        impl->policy.Delivered(pending.turn.thread, text, realMs);
                        ++impl->replies;
                    }
                    else
                        ++impl->omitted;
                    impl->nextDelivery = realMs + 1000;
                    LOG_INFO("module.alles", "Conversation {} delivered={} bot={} human={} action={} actionOk={}",
                             result.id, delivered, pending.candidate.bot, pending.turn.human, action, failure.empty());
                    if (impl->recorder)
                        impl->recorder->Record({ActorKind::Player, pending.candidate.bot}, "alles_conversation",
                            delivered ? 1 : 0, text,
                            Acore::StringFormat("human={} channel={} action={} actionOk={} job={} "
                                "thread={} speakerIsBot={}",
                                pending.turn.human, SpeechRouteLabel(pending.turn.route), action, failure.empty(),
                                result.id, pending.turn.thread, pending.turn.sourceBot), realMs);
                }
                else if (result.status != "success" || result.response.at("reply").as_bool())
                    ++impl->omitted;
            }
            impl->policy.Finish(pending.turn.thread);
            impl->pending.erase(found);
        }
    }
    for (auto turn = impl->turns.begin(); turn != impl->turns.end();)
    {
        if (realMs - turn->admitted >= turn->Lifetime() || impl->generations[turn->human] != turn->generation)
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
        if (!turn->elected && !turn->candidates.empty())
        {
            std::vector<ReplyCandidate> scores;
            std::optional<uint64_t> addressed;
            auto const name = AddressedBot(turn->text, turn->audience);
            for (auto const& candidate : turn->candidates)
            {
                if (name && candidate.name == *name)
                    addressed = candidate.bot;
                if (!impl->policy.CanReply(turn->thread, turn->human, candidate.bot, realMs))
                    continue;
                auto* bot = Find(candidate.bot);
                if (!bot || !impl->Valid({*turn, candidate, {}}))
                    continue;
                if (turn->recruitment)
                {
                    if (!impl->objectives || !impl->objectives->HelpOfferContext({ActorKind::Player, candidate.bot},
                        candidate.actorGeneration, *turn->recruitment, gameMs, realMs).at("canOfferHelp").as_bool())
                        continue;
                }
                unsigned relevance = 1;
                if (auto const* snapshot = impl->store.FindReady({ActorKind::Player, candidate.bot}))
                    relevance += RetrieveConversationKnowledge(*snapshot, turn->text,
                        uint8_t(bot->GetLevel()), gameMs).relevance;
                scores.push_back({candidate.bot, relevance});
            }
            std::vector<Impl::Candidate> elected;
            unsigned const limit = turn->recruitment ? turn->recruitment->question.partySize - 1 : 1;
            while (elected.size() < limit)
            {
                auto selected = ElectRespondent(scores, addressed, turn->human + turn->emitted + elected.size());
                if (!selected)
                    break;
                auto found = std::find_if(turn->candidates.begin(), turn->candidates.end(),
                    [&](auto const& candidate) { return candidate.bot == *selected; });
                elected.push_back(*found);
                std::erase_if(scores, [&](auto const& score) { return score.actor == *selected; });
            }
            turn->candidates = std::move(elected);
            if (!turn->candidates.empty())
                turn->dispatchAfter = turn->admitted + 250 + (turn->candidates.front().bot % 751);
            turn->elected = true;
        }
        if (realMs < turn->dispatchAfter)
        {
            ++turn;
            continue;
        }
        for (auto candidate = turn->candidates.begin(); candidate != turn->candidates.end();)
        {
            if (turn->recruitment)
                if (auto const* thread = impl->policy.Find(turn->thread); thread && thread->pending)
                    break;
            if (auto addressed = AddressedBot(turn->text, turn->audience); addressed && *addressed != candidate->name)
            {
                candidate = turn->candidates.erase(candidate);
                continue;
            }
            bool const busy = std::any_of(
                impl->pending.begin(), impl->pending.end(), [&](auto const& item)
                { return item.second.candidate.bot == candidate->bot; });
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
                if (auto* threat = IsRemoteSpeech(turn->route.type) ? nullptr : HumanRequestThreat(bot, human))
                    pending.threat = threat->GetGUID();
                auto context = impl->Context(*turn, *candidate, bot, human);
                if (!BoundConversationContext(context))
                {
                    ++impl->omitted;
                    candidate = turn->candidates.erase(candidate);
                    continue;
                }
                auto id = "conversation-" + std::to_string(gameMs) + "-" + std::to_string(++impl->nextJob);
                if (!impl->policy.Reserve(turn->thread, turn->human, candidate->bot, realMs))
                {
                    ++impl->omitted;
                    candidate = turn->candidates.erase(candidate);
                    continue;
                }
                if (!impl->bridge.QueueConversation(id, std::move(context), realMs,
                    {ActorKind::Player, candidate->bot}, candidate->generation, std::to_string(turn->thread)))
                {
                    impl->policy.Finish(turn->thread);
                    ++impl->omitted;
                    candidate = turn->candidates.erase(candidate);
                    continue;
                }
                impl->Remember(turn->human, candidate->bot, turn->route, human.GetName(), turn->text, realMs);
                impl->pending.emplace(id, std::move(pending));
            }
            candidate = turn->candidates.erase(candidate);
        }
        if (turn->candidates.empty())
            turn = impl->turns.erase(turn);
        else
            ++turn;
    }

}

void ConversationRuntime::Stop()
{
    for (auto const& [id, pending] : impl->pending)
        impl->bridge.CancelConversation(id);
    impl->pending.clear();
    impl->turns.clear();
    impl->questions.clear();
    impl->informationReplies.clear();
}

boost::json::object ConversationRuntime::Status() const
{
    return {{"enabled", true},          {"queuedTurns", impl->turns.size()}, {"pendingReplies", impl->pending.size()},
            {"replies", impl->replies}, {"actions", impl->actions},
            {"following", (impl->objectives ? impl->objectives->FollowingCount() : 0)},
            {"omitted", impl->omitted}, {"threads", impl->policy.Size()},
            {"questions", impl->questions.size()}, {"questionsAttempted", impl->questionsAttempted},
            {"questionsDelivered", impl->questionsDelivered}, {"informationReplies", impl->informationReplies.size()}};
}
} // namespace Alles
