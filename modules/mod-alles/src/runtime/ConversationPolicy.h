/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_CONVERSATION_POLICY_H
#define MOD_ALLES_CONVERSATION_POLICY_H

#include "domain/Knowledge.h"
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Alles
{
std::string ConversationFingerprint(std::string_view text);
std::optional<Activity> ConversationActivity(std::string_view text);

struct ReplyCandidate
{
    uint64_t actor = 0;
    unsigned relevance = 0;
};

// Chooses among actual listeners, preferring useful private evidence. Never copies that evidence to peers.
std::optional<uint64_t> ElectRespondent(std::vector<ReplyCandidate> const& candidates,
    std::optional<uint64_t> addressed, uint64_t rotation);

struct DialogueThread
{
    uint64_t id = 0;
    uint64_t origin = 0;
    uint64_t respondent = 0;
    uint64_t openedMs = 0;
    uint64_t expiresMs = 0;
    uint32_t attempts = 0;
    bool pending = false;
    std::string scope;
    std::string topic;
    bool initiated = false;
    uint8_t recruitmentLimit = 0;
    std::set<uint64_t> recruitmentRespondents;
};

// Ephemeral social scheduling, separate from semantic intentions. Budgets count attempts, not model success.
// Caller fences live session generations/membership. One pending reply; ordinary threads have two participants.
// Recruitment permits one offer each from up to four distinct respondents, within the same attempt budgets.
class ConversationPolicy
{
public:
    std::optional<uint64_t> Open(uint64_t origin, std::string scope, std::string_view text, uint64_t now);
    bool CanReply(uint64_t thread, uint64_t speaker, uint64_t respondent, uint64_t now) const;
    bool Reserve(uint64_t thread, uint64_t speaker, uint64_t respondent, uint64_t now);
    bool ReserveQuestion(uint64_t thread, uint64_t now);
    bool EnableRecruitment(uint64_t thread, uint8_t respondents);
    void Finish(uint64_t thread);
    bool DuplicateAnswer(uint64_t thread, std::string_view text, uint64_t now) const;
    void Delivered(uint64_t thread, std::string_view text, uint64_t now);
    void Forget(uint64_t actor);
    void Prune(uint64_t now);
    DialogueThread const* Find(uint64_t thread) const;
    std::size_t Size() const { return _threads.size(); }

private:
    struct Answer
    {
        std::string scope;
        std::string text;
        uint64_t expiresMs = 0;
    };
    uint64_t _next = 1;
    std::map<uint64_t, DialogueThread> _threads;
    std::map<uint64_t, std::deque<uint64_t>> _actors;
    std::map<std::string, std::deque<uint64_t>> _audiences;
    std::deque<Answer> _answers;
};
}

#endif
