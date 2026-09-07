/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Wire.h"
#include <boost/json/src.hpp>
#include <set>
#include <stdexcept>

namespace Alles::Bridge
{
namespace
{
// The DOM parser validates JSON syntax; this independent walk rejects duplicate decoded keys
// before the DOM's last-value-wins behavior could hide one. Nesting and bytes are bounded first.
class Keys
{
  public:
    explicit Keys(std::string_view value) : text(value) {}
    void Space()
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
    }
    std::string Quoted()
    {
        auto start = pos++;
        while (pos < text.size())
        {
            char c = text[pos++];
            if (c == '\\')
                ++pos;
            else if (c == '"')
                break;
        }
        return std::string(boost::json::parse(text.substr(start, pos - start)).as_string());
    }
    void Value()
    {
        Space();
        char c = text[pos];
        if (c == '"')
        {
            Quoted();
            return;
        }
        if (c != '{' && c != '[')
        {
            while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']')
                ++pos;
            return;
        }
        ++pos;
        Space();
        char end = c == '{' ? '}' : ']';
        std::set<std::string> seen;
        while (text[pos] != end)
        {
            if (c == '{')
            {
                if (!seen.insert(Quoted()).second)
                    throw std::invalid_argument("duplicate JSON key");
                Space();
                ++pos;
            }
            Value();
            Space();
            if (text[pos] == ',')
            {
                ++pos;
                Space();
            }
            else
                break;
        }
        ++pos;
    }
    std::string_view text;
    std::size_t pos = 0;
};
} // namespace

boost::json::value Parse(std::string_view text)
{
    if (text.empty() || text.size() > 65536)
        throw std::invalid_argument("JSON frame size");
    boost::json::parse_options options;
    options.max_depth = 16;
    auto value = boost::json::parse(text, {}, options);
    Keys(text).Value();
    return value;
}

void Fields(boost::json::object const& object, std::initializer_list<char const*> fields)
{
    if (object.size() != fields.size())
        throw std::invalid_argument("unexpected JSON fields");
    for (auto field : fields)
        if (!object.contains(field))
            throw std::invalid_argument("missing JSON field");
}

std::string String(boost::json::object const& object, char const* key, std::size_t max)
{
    auto const& value = object.at(key).as_string();
    if (value.size() > max || (!value.empty() && !IsBoundedText(std::string_view(value), max)))
        throw std::invalid_argument("invalid string");
    return std::string(value);
}

uint64_t Number(boost::json::object const& object, char const* key)
{
    auto const& value = object.at(key);
    if (value.is_uint64())
        return value.as_uint64();
    if (value.is_int64() && value.as_int64() >= 0)
        return uint64_t(value.as_int64());
    throw std::invalid_argument("expected unsigned integer");
}

boost::json::object EncodeJob(Interpreter::JobSnapshot const& job, uint64_t realMs)
{
    boost::json::array perceptions, memories, entities, places, draft;
    for (auto const& m : Interpreter::MakeFakeProposal(job, {}).memories)
    {
        boost::json::array support;
        for (auto const& token : m.supportingPerceptions)
            support.emplace_back(token);
        draft.emplace_back(boost::json::object{{"operation", uint8_t(m.operation)},
                                               {"kind", uint8_t(m.kind)},
                                               {"supportingPerceptions", support},
                                               {"targetMemoryToken", m.targetMemoryToken},
                                               {"subjectToken", m.subjectToken},
                                               {"sourceToken", m.sourceToken},
                                               {"placeToken", m.placeToken},
                                               {"text", m.text},
                                               {"confidence", m.confidence},
                                               {"salience", m.salience}});
    }
    for (auto const& input : job.perceptions)
    {
        auto const& p = input.value;
        perceptions.emplace_back(boost::json::object{{"token", input.token},
                                                     {"kind", uint8_t(p.kind)},
                                                     {"subjectToken", input.subjectToken},
                                                     {"sourceToken", input.sourceToken},
                                                     {"placeToken", input.placeToken},
                                                     {"text", p.text},
                                                     {"selfContext", p.selfContext},
                                                     {"comprehended", p.comprehended}});
    }
    for (auto const& input : job.memories)
    {
        auto const& m = input.value;
        memories.emplace_back(boost::json::object{{"token", input.token},
                                                  {"kind", uint8_t(m.kind)},
                                                  {"claim", m.claim},
                                                  {"attribution", m.attribution},
                                                  {"confidence", m.confidence},
                                                  {"salience", m.salience}});
    }
    for (auto const& e : job.entities)
        entities.emplace_back(boost::json::object{{"token", e.token}, {"name", e.value.name}});
    for (auto const& p : job.places)
        places.emplace_back(boost::json::object{{"token", p.token}, {"name", p.name}});
    return {{"bootEpoch", job.bootEpoch},
            {"jobToken", job.jobToken},
            {"ownerKind", uint8_t(job.owner.kind)},
            {"ownerId", job.owner.id},
            {"actorGeneration", job.actorGeneration},
            {"workerId", job.workerId},
            {"profileFingerprint", job.profileFingerprint},
            {"leaseGeneration", job.leaseGeneration},
            {"remainingMs", job.admittedRealTimeMs + 45000 > realMs ? job.admittedRealTimeMs + 45000 - realMs : 0},
            {"perceptions", perceptions},
            {"memories", memories},
            {"entities", entities},
            {"places", places},
            {"draft", boost::json::object{{"memories", draft}}}};
}

Interpreter::ProposalEnvelope DecodeProposal(boost::json::object const& object)
{
    Fields(object, {"bootEpoch", "jobToken", "ownerKind", "ownerId", "actorGeneration", "workerId",
                    "profileFingerprint", "leaseGeneration", "permitId", "memories"});
    Interpreter::ProposalEnvelope result;
    result.bootEpoch = String(object, "bootEpoch", 64);
    result.jobToken = String(object, "jobToken", 64);
    if (Number(object, "ownerKind") != 0)
        throw std::invalid_argument("pilot owner kind");
    result.owner = {ActorKind::Player, Number(object, "ownerId")};
    result.actorGeneration = Number(object, "actorGeneration");
    result.workerId = String(object, "workerId", 64);
    result.profileFingerprint = String(object, "profileFingerprint", 128);
    result.leaseGeneration = Number(object, "leaseGeneration");
    result.permitId = String(object, "permitId", 64);
    auto const& proposals = object.at("memories").as_array();
    if (proposals.size() > 4)
        throw std::invalid_argument("proposal count");
    for (auto const& value : proposals)
    {
        auto const& m = value.as_object();
        Fields(m, {"operation", "supportingPerceptions", "targetMemoryToken", "subjectToken", "sourceToken",
                   "placeToken", "text", "confidence", "salience", "kind"});
        Interpreter::MemoryProposal p;
        if (Number(m, "operation") > 2 || Number(m, "kind") > 5)
            throw std::invalid_argument("proposal enum");
        p.operation = Interpreter::ProposalOperation(Number(m, "operation"));
        p.kind = MemoryKind(Number(m, "kind"));
        auto const& support = m.at("supportingPerceptions").as_array();
        if (support.size() > 8)
            throw std::invalid_argument("support count");
        for (auto const& token : support)
        {
            if (token.as_string().size() > 64)
                throw std::invalid_argument("support token");
            p.supportingPerceptions.emplace_back(token.as_string());
        }
        p.targetMemoryToken = String(m, "targetMemoryToken", 64);
        p.subjectToken = String(m, "subjectToken", 64);
        p.sourceToken = String(m, "sourceToken", 64);
        p.placeToken = String(m, "placeToken", 64);
        p.text = String(m, "text", 2048);
        p.confidence = boost::json::value_to<double>(m.at("confidence"));
        p.salience = boost::json::value_to<double>(m.at("salience"));
        result.memories.push_back(std::move(p));
    }
    return result;
}
} // namespace Alles::Bridge
