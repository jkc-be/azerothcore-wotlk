/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_CONVERSATION_KNOWLEDGE_H
#define MOD_ALLES_CONVERSATION_KNOWLEDGE_H

#include "domain/ActorStore.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

namespace Alles
{
struct ConversationKnowledge
{
    boost::json::array places;
    boost::json::array reports;
    boost::json::array memories;
    unsigned relevance = 0;
};

// Only this owner's snapshot is accepted. Typed activities enrich retrieval; lexical matching is secondary.
ConversationKnowledge RetrieveConversationKnowledge(OwnerSnapshot const& owner, std::string const& message,
    uint8_t level, uint64_t gameMs);
// Leaves room for the worker's contract within its 12 KiB request bound, including JSON escaping.
bool BoundConversationContext(boost::json::object& context, std::size_t maxBytes = 8000);
}

#endif
