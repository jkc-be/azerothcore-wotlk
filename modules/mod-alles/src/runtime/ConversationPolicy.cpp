/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "ConversationPolicy.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace Alles
{
namespace
{
constexpr uint64_t WindowMs = 60000;
constexpr uint64_t ThreadMs = 120000;

std::set<std::string> Terms(std::string_view text)
{
    std::set<std::string> result;
    auto value = ConversationFingerprint(text);
    std::size_t start = 0;
    while (start < value.size())
    {
        auto end = value.find(' ', start);
        if (end == std::string::npos)
            end = value.size();
        result.insert(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

bool Similar(std::string const& left, std::string const& right)
{
    if (left == right)
        return true;
    auto const a = Terms(left);
    auto const b = Terms(right);
    std::size_t shared = 0;
    for (auto const& term : a)
        shared += b.contains(term);
    // Short replies such as yes/no are only duplicates when identical. Longer paraphrases with nearly
    // identical words are suppressed; semantic novelty beyond this remains the worker's judgment.
    return shared >= 5 && shared * 5 >= (a.size() + b.size() - shared) * 4;
}
}

std::string ConversationFingerprint(std::string_view text)
{
    std::string result;
    bool space = false;
    for (unsigned char c : text)
    {
        if (c >= 128 || std::isalnum(c))
        {
            if (space && !result.empty())
                result += ' ';
            result += c < 128 ? char(std::tolower(c)) : char(c);
            space = false;
        }
        else
            space = true;
    }
    return result;
}

std::optional<Activity> ConversationActivity(std::string_view text)
{
    auto const words = Terms(text);
    auto any = [&](std::initializer_list<char const*> choices)
    {
        return std::any_of(choices.begin(), choices.end(), [&](auto const* word) { return words.contains(word); });
    };
    if (any({"party", "group", "companion", "companions", "healer", "tank", "together"}))
        return Activity::Companions;
    if (any({"supplies", "food", "drink", "equipment", "vendor", "repair"}))
        return Activity::Supplies;
    if (any({"hunt", "hunting", "prey", "fight", "fighting"}))
        return Activity::Hunt;
    if (any({"travel", "road", "route", "directions", "explore"}))
        return Activity::Travel;
    if (any({"work", "quest", "quests", "task", "tasks", "level", "leveling"}))
        return Activity::Work;
    // This enriches retrieval; it is never a required English trigger for admitting a conversation.
    return std::nullopt;
}

std::optional<uint64_t> ElectRespondent(std::vector<ReplyCandidate> const& candidates,
    std::optional<uint64_t> addressed, uint64_t rotation)
{
    std::vector<uint64_t> eligible;
    unsigned best = 0;
    for (auto const& candidate : candidates)
    {
        if (!candidate.actor || (addressed && candidate.actor != *addressed))
            continue;
        if (addressed)
            return candidate.actor;
        if (!candidate.relevance || candidate.relevance < best)
            continue;
        if (candidate.relevance > best)
        {
            best = candidate.relevance;
            eligible.clear();
        }
        eligible.push_back(candidate.actor);
    }
    if (eligible.empty())
        return std::nullopt;
    std::sort(eligible.begin(), eligible.end());
    eligible.erase(std::unique(eligible.begin(), eligible.end()), eligible.end());
    return eligible[rotation % eligible.size()];
}

std::optional<uint64_t> ConversationPolicy::Open(uint64_t origin, std::string scope,
    std::string_view text, uint64_t now)
{
    Prune(now);
    auto topic = ConversationFingerprint(text);
    if (!origin || scope.empty() || !IsBoundedText(scope, 512) || text.size() > 255 || topic.empty()
        || !IsBoundedText(text, 255) || _threads.size() >= 128
        || _next == std::numeric_limits<uint64_t>::max() || now > std::numeric_limits<uint64_t>::max() - ThreadMs)
        return std::nullopt;
    for (auto const& [id, thread] : _threads)
        if (thread.scope == scope && thread.topic == topic)
            return std::nullopt;
    uint64_t const id = _next++;
    _threads.emplace(id, DialogueThread{id, origin, 0, now, now + ThreadMs, 0, false,
        std::move(scope), std::move(topic)});
    _threads.at(id).greeting = IsRoutineGreeting(text);
    return id;
}

bool ConversationPolicy::CanReply(uint64_t id, uint64_t speaker, uint64_t respondent, uint64_t now) const
{
    auto const* thread = Find(id);
    if (!thread || !speaker || !respondent || speaker == respondent || now < thread->openedMs
        || now >= thread->expiresMs || thread->pending || thread->closed || thread->attempts >= 4)
        return false;
    if (thread->recruitmentLimit)
    {
        if (speaker != thread->origin || thread->recruitmentRespondents.contains(respondent)
            || thread->recruitmentRespondents.size() >= thread->recruitmentLimit)
            return false;
    }
    else if (thread->respondent)
    {
        if (!((speaker == thread->origin && respondent == thread->respondent)
            || (speaker == thread->respondent && respondent == thread->origin)))
            return false;
    }
    else if (speaker != thread->origin)
        return false;
    auto actor = _actors.find(respondent);
    auto audience = _audiences.find(thread->scope);
    auto recent = [now](auto const& times)
    {
        return std::count_if(times.begin(), times.end(), [now](uint64_t time)
            { return time > now || now - time < WindowMs; });
    };
    return (actor == _actors.end() || recent(actor->second) < 4)
        && (audience == _audiences.end() || recent(audience->second) < 6);
}

bool ConversationPolicy::Reserve(uint64_t id, uint64_t speaker, uint64_t respondent, uint64_t now)
{
    Prune(now);
    if (!CanReply(id, speaker, respondent, now))
        return false;
    auto& thread = _threads.at(id);
    if ((!_actors.contains(respondent) && _actors.size() >= 256)
        || (!_audiences.contains(thread.scope) && _audiences.size() >= 128))
        return false;
    _actors[respondent].push_back(now);
    _audiences[thread.scope].push_back(now);
    if (thread.recruitmentLimit)
        thread.recruitmentRespondents.insert(respondent);
    else if (!thread.respondent)
        thread.respondent = respondent;
    thread.pending = true;
    ++thread.attempts;
    return true;
}

void ConversationPolicy::Finish(uint64_t id)
{
    if (auto found = _threads.find(id); found != _threads.end())
        found->second.pending = false;
}

bool ConversationPolicy::EnableRecruitment(uint64_t id, uint8_t respondents)
{
    auto found = _threads.find(id);
    if (found == _threads.end() || !respondents || respondents > 4 || !found->second.initiated
        || found->second.attempts || found->second.respondent || found->second.recruitmentLimit)
        return false;
    found->second.recruitmentLimit = respondents;
    return true;
}

bool ConversationPolicy::ReserveQuestion(uint64_t id, uint64_t now)
{
    Prune(now);
    auto found = _threads.find(id);
    if (found == _threads.end())
        return false;
    auto& thread = found->second;
    if (thread.initiated || thread.attempts || thread.respondent || now < thread.openedMs
        || (!_actors.contains(thread.origin) && _actors.size() >= 256)
        || (!_audiences.contains(thread.scope) && _audiences.size() >= 128))
        return false;
    auto& actor = _actors[thread.origin];
    auto& audience = _audiences[thread.scope];
    if (actor.size() >= 4 || audience.size() >= 6)
        return false;
    actor.push_back(now);
    audience.push_back(now);
    thread.initiated = true;
    return true;
}

bool ConversationPolicy::DuplicateAnswer(uint64_t id, std::string_view text, uint64_t now) const
{
    auto const* thread = Find(id);
    if (!thread)
        return true;
    if (thread->recruitmentLimit)
        return false; // Different people's willing offers are distinct evidence, even with identical wording.
    auto const normalized = ConversationFingerprint(text);
    return std::any_of(_answers.begin(), _answers.end(), [&](auto const& answer)
    {
        return answer.expiresMs > now && answer.scope == thread->scope && Similar(answer.text, normalized);
    });
}

void ConversationPolicy::Delivered(uint64_t id, std::string_view text, uint64_t now)
{
    auto const* thread = Find(id);
    if (!thread || text.size() > 255 || now > std::numeric_limits<uint64_t>::max() - WindowMs)
        return;
    if (_answers.size() >= 128)
        _answers.pop_front();
    _answers.push_back({thread->scope, ConversationFingerprint(text), now + WindowMs});
    if (thread->greeting && IsRoutineGreeting(text))
        _threads.at(id).closed = true;
}

void ConversationPolicy::Forget(uint64_t actor)
{
    std::erase_if(_threads, [actor](auto const& entry)
        { return entry.second.origin == actor || entry.second.respondent == actor
            || entry.second.recruitmentRespondents.contains(actor); });
    // Relog does not refund attempt budgets or erase recently delivered answer suppression.
}

void ConversationPolicy::Prune(uint64_t now)
{
    std::erase_if(_threads, [now](auto const& entry) { return entry.second.expiresMs <= now; });
    std::erase_if(_answers, [now](auto const& answer) { return answer.expiresMs <= now; });
    auto prune = [now](auto& budgets)
    {
        for (auto& [key, times] : budgets)
            std::erase_if(times, [now](uint64_t time) { return time <= now && now - time >= WindowMs; });
        std::erase_if(budgets, [](auto const& entry) { return entry.second.empty(); });
    };
    prune(_actors);
    prune(_audiences);
}

DialogueThread const* ConversationPolicy::Find(uint64_t id) const
{
    auto const found = _threads.find(id);
    return found == _threads.end() ? nullptr : &found->second;
}
}
