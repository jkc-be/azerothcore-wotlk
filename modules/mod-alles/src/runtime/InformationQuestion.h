/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_INFORMATION_QUESTION_H
#define MOD_ALLES_INFORMATION_QUESTION_H

#include "domain/Knowledge.h"

namespace Alles
{
struct InformationQuestion
{
    ActorKey owner;
    uint64_t generation = 0;
    uint64_t objective = 0;
    uint64_t revision = 0;
    uint32_t attempt = 0;
    Association topic;
    std::string text;
    std::string questName;
    std::string placeName;
    uint8_t partySize = 2;
};

struct InformationReply
{
    InformationQuestion question;
    Reference source;
    std::string text;
    std::string channel;
    uint64_t gameMs = 0;
    uint64_t realMs = 0;
};

// Constructed only from an actually delivered recruitment utterance, never from the speaker's private quest log.
struct RecruitmentNotice
{
    InformationQuestion question;
    Reference source;
    uint64_t receivedMs = 0;
};

struct CooperativeRoster
{
    ActorKey leader;
    uint32_t quest = 0;
    uint32_t place = 0;
    std::vector<Reference> members;
    std::string text;
};
}

#endif
