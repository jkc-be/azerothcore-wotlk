/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#include "ConversationKnowledge.h"
#include "ConversationPolicy.h"
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <set>

namespace Alles
{
bool BoundConversationContext(boost::json::object& context, std::size_t maxBytes)
{
    for (std::size_t const keep : {std::size_t(1), std::size_t(0)})
        for (auto const* name : {"audience", "relevantMemories", "history", "knownPlaces", "learnedReports"})
        {
            auto* value = context.if_contains(name);
            if (!value || !value->is_array())
                continue;
            auto& entries = value->as_array();
            while (entries.size() > keep && boost::json::serialize(context).size() > maxBytes)
            {
                if (std::string_view(name) == "history")
                    entries.erase(entries.begin());
                else
                    entries.pop_back();
            }
        }
    return boost::json::serialize(context).size() <= maxBytes;
}

namespace
{
bool Mentions(std::string const& message, std::string const& name)
{
    auto const label = ConversationFingerprint(name);
    return !label.empty() && (" " + ConversationFingerprint(message) + " ").find(" " + label + " ")
        != std::string::npos;
}
}

ConversationKnowledge RetrieveConversationKnowledge(OwnerSnapshot const& owner, std::string const& message,
    uint8_t level, uint64_t gameMs)
{
    ConversationKnowledge result;
    auto const activity = ConversationActivity(message);
    Association topic;
    topic.activity = activity.value_or(Activity::Work);
    if (owner.planning)
    {
        PrivateKnowledge knowledge;
        if (knowledge.Restore(owner.planning->knowledge))
        {
            std::vector<KnownPlace const*> places;
            for (auto const& [id, place] : knowledge.Places())
            {
                bool const named = Mentions(message, place.name);
                if (named)
                    topic.place = id;
                if (named || !place.minimumLevel || (place.minimumLevel <= level + 2 && place.maximumLevel >= level))
                    places.push_back(&place);
            }
            std::sort(places.begin(), places.end(), [&](auto const* left, auto const* right)
            {
                bool const leftNamed = Mentions(message, left->name);
                bool const rightNamed = Mentions(message, right->name);
                if (leftNamed != rightNamed)
                    return leftNamed;
                if (left->lastUsefulWorkMs != right->lastUsefulWorkMs)
                    return left->lastUsefulWorkMs > right->lastUsefulWorkMs;
                return left->area < right->area;
            });
            for (auto const* place : places)
            {
                if (result.places.size() == 6)
                    break;
                auto relative = knowledge.Places().find(place->relativeTo);
                result.places.emplace_back(boost::json::object{
                    {"id", place->area}, {"name", place->name}, {"approximateMinimumLevel", place->minimumLevel},
                    {"approximateMaximumLevel", place->maximumLevel}, {"direction", place->direction},
                    {"relativeTo", relative == knowledge.Places().end() ? "" : relative->second.name},
                    {"origin", place->origin == KnowledgeOrigin::Starting ? "vague starting geography"
                        : place->origin == KnowledgeOrigin::Experienced ? "my experience" : "a report"},
                    {"visitedMs", place->visitedMs}, {"usefulWorkMs", place->lastUsefulWorkMs}});
                if (topic.place == place->area || (activity && (*activity == Activity::Work
                    || *activity == Activity::Hunt || *activity == Activity::Travel)))
                    result.relevance = std::max(result.relevance,
                        place->lastUsefulWorkMs ? 8u : place->visitedMs ? 4u : 1u);
            }
            for (auto const* report : knowledge.Retrieve(topic, gameMs, 4))
            {
                result.reports.emplace_back(boost::json::object{
                    {"source", report->source.name}, {"text", report->text}, {"receivedMs", report->receivedMs},
                    {"confidence", report->confidence}, {"place", report->topic.place},
                    {"quest", report->topic.quest}, {"activity", uint8_t(report->topic.activity)},
                    {"usefulVisits", report->usefulVisits}, {"unsuccessfulVisits", report->unsuccessfulVisits}});
                if (activity || topic.place)
                    result.relevance = std::max(result.relevance, 6u);
            }
        }
    }
    std::vector<Memory const*> memories;
    auto const words = ConversationFingerprint(message);
    for (auto const& memory : owner.memories)
    {
        auto text = RenderMemory(memory);
        if (text.size() > 512)
            continue;
        auto const terms = ConversationFingerprint(text);
        bool match = false;
        std::size_t start = 0;
        while (start < words.size() && !match)
        {
            auto end = words.find(' ', start);
            if (end == std::string::npos)
                end = words.size();
            if (end - start >= 4)
                match = (" " + terms + " ").find(" " + words.substr(start, end - start) + " ") != std::string::npos;
            start = end + 1;
        }
        if (match)
            memories.push_back(&memory);
    }
    std::stable_sort(memories.begin(), memories.end(), [](auto const* left, auto const* right)
    {
        return std::min(left->salience, SalienceCeiling(*left)) > std::min(right->salience, SalienceCeiling(*right));
    });
    std::set<std::string> included;
    for (auto const* memory : memories)
    {
        auto text = RenderMemory(*memory);
        if (!included.insert(text).second)
            continue;
        result.memories.emplace_back(text);
        if (SalienceCeiling(*memory) > 0.05)
            result.relevance = std::max(result.relevance, 3u);
        if (result.memories.size() == 4)
            break;
    }
    return result;
}
}
