/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Knowledge.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Alles
{
namespace
{
KnownPlace Place(uint32_t area, std::string name, uint8_t minimum, uint8_t maximum,
    uint32_t relativeTo, std::string direction)
{
    return {area, std::move(name), minimum, maximum, relativeTo, std::move(direction)};
}

unsigned Relevance(Association const& requested, Association const& known)
{
    unsigned score = requested.activity == known.activity ? 1 : 0;
    if (requested.quest && requested.quest == known.quest)
        score += 8;
    if (requested.place && requested.place == known.place)
        score += 4;
    if (requested.person && requested.person == known.person)
        score += 2;
    return score;
}

bool ValidRepairLocation(RepairLocation const& location)
{
    return location.phase && location.observedMs && std::isfinite(location.x) && std::isfinite(location.y)
        && std::isfinite(location.z) && std::abs(location.x) <= 32768 && std::abs(location.y) <= 32768
        && std::abs(location.z) <= 32768;
}

float DistanceSquared(RepairLocation const& location, float x, float y, float z)
{
    return (location.x - x) * (location.x - x) + (location.y - y) * (location.y - y)
        + (location.z - z) * (location.z - z);
}
}

std::optional<StartingProfile> StartingGeography(uint8_t race)
{
    // Identities verified against the installed 3.3.5a AreaTable.dbc and local world data, 2026-09-07.
    // Directions are coarse connections, level bands approximate. No quest/NPC/loot/coordinate seeds.
    switch (race)
    {
        case 1:
            return StartingProfile{1, "Northshire", {
                Place(9, "Northshire Valley", 1, 5, 0, "the surrounding valley"),
                Place(87, "Goldshire", 5, 10, 9, "southwest, outside the valley"),
                Place(12, "Elwynn Forest", 1, 10, 9, "the forest around the valley")}};
        case 3:
        case 7:
            return StartingProfile{1, "Coldridge", {
                Place(132, "Coldridge Valley", 1, 5, 0, "the surrounding valley"),
                Place(131, "Kharanos", 5, 10, 132, "northeast, beyond the pass"),
                Place(1, "Dun Morogh", 1, 10, 132, "the snowy country beyond the valley")}};
        case 4:
            return StartingProfile{1, "Shadowglen", {
                Place(188, "Shadowglen", 1, 5, 0, "the surrounding glen"),
                Place(186, "Dolanaar", 5, 10, 188, "south of the glen"),
                Place(141, "Teldrassil", 1, 10, 188, "the wider woodland around the glen")}};
        case 11:
            return StartingProfile{1, "Ammen Vale", {
                Place(3526, "Ammen Vale", 1, 5, 0, "the surrounding vale"),
                Place(3576, "Azure Watch", 5, 10, 3526, "west, outside the vale"),
                Place(3524, "Azuremyst Isle", 1, 10, 3526, "the island around the vale")}};
        case 2:
        case 8:
            return StartingProfile{1, "Valley of Trials", {
                Place(363, "Valley of Trials", 1, 5, 0, "the surrounding valley"),
                Place(367, "Sen'jin Village", 5, 10, 363, "southeast toward the coast"),
                Place(362, "Razor Hill", 5, 10, 363, "northeast of the valley"),
                Place(14, "Durotar", 1, 10, 363, "the dry country outside the valley")}};
        case 6:
            return StartingProfile{1, "Red Cloud Mesa", {
                Place(220, "Red Cloud Mesa", 1, 5, 0, "the surrounding mesa"),
                Place(222, "Bloodhoof Village", 5, 10, 220, "north, down from the mesa"),
                Place(215, "Mulgore", 1, 10, 220, "the plains around the mesa")}};
        case 5:
            return StartingProfile{1, "Deathknell", {
                Place(154, "Deathknell", 1, 5, 0, "the surrounding settlement"),
                Place(159, "Brill", 5, 10, 154, "northeast, outside the valley"),
                Place(85, "Tirisfal Glades", 1, 10, 154, "the glades beyond the settlement")}};
        case 10:
            return StartingProfile{1, "Sunstrider Isle", {
                Place(3431, "Sunstrider Isle", 1, 5, 0, "the surrounding isle"),
                Place(3665, "Falconwing Square", 5, 10, 3431, "southeast, beyond the isle"),
                Place(3462, "Fairbreeze Village", 5, 10, 3431, "south, farther into the woods"),
                Place(3430, "Eversong Woods", 1, 10, 3431, "the woodland beyond the isle")}};
        default:
            return std::nullopt;
    }
}

bool IsValidKnowledgeSnapshot(KnowledgeSnapshot const& snapshot)
{
    if (snapshot.seedVersion > 1 || !snapshot.nextReport
        || snapshot.nextReport == std::numeric_limits<uint64_t>::max()
        || snapshot.places.size() > 64 || snapshot.reports.size() > 64)
        return false;
    for (auto const& [id, place] : snapshot.places)
        if (!id || id != place.area || place.name.empty() || !IsBoundedText(place.name, 100)
            || !IsBoundedText(place.direction, 100) || place.origin > KnowledgeOrigin::Reported
            || place.minimumLevel > place.maximumLevel || place.maximumLevel > 80
            || (bool(place.minimumLevel) != bool(place.maximumLevel))
            || (place.repair && (!place.visitedMs || !ValidRepairLocation(*place.repair))))
            return false;
    for (auto const& [id, report] : snapshot.reports)
        if (!id || id != report.id || id >= snapshot.nextReport || report.text.empty()
            || !IsBoundedText(report.text, 512) || !IsBoundedText(report.source.name, 100)
            || (!report.source.actor && report.source.name.empty())
            || (report.source.actor && !IsValidActor(*report.source.actor))
            || (report.topic.person && !IsValidActor(*report.topic.person)) || report.topic.activity > Activity::Travel
            || !std::isfinite(report.confidence) || report.confidence < 0 || report.confidence > 0.6)
            return false;
    return true;
}

KnowledgeSnapshot PrivateKnowledge::Capture() const
{
    return {_seedVersion, _nextReport, _places, _reports};
}

bool PrivateKnowledge::Restore(KnowledgeSnapshot snapshot)
{
    if (!IsValidKnowledgeSnapshot(snapshot))
        return false;
    _seedVersion = snapshot.seedVersion;
    _nextReport = snapshot.nextReport;
    _places = std::move(snapshot.places);
    _reports = std::move(snapshot.reports);
    return true;
}

bool PrivateKnowledge::Seed(uint8_t race, bool deathKnight, bool startingExperienceComplete)
{
    if (_seedVersion || (deathKnight && !startingExperienceComplete))
        return false;
    auto profile = StartingGeography(race);
    if (!profile)
        return false;
    auto places = _places;
    for (auto& place : profile->places)
        places.try_emplace(place.area, std::move(place));
    if (places.size() > 64)
        return false;
    _places = std::move(places);
    _seedVersion = profile->version;
    return true;
}

bool PrivateKnowledge::LearnPlace(uint32_t area, std::string name)
{
    if (!area || name.empty() || !IsBoundedText(name, 100))
        return false;
    if (_places.contains(area))
        return true; // Hearsay never replaces established geography or personal visits.
    if (_places.size() >= 64)
        return false;
    _places.emplace(area, KnownPlace{area, std::move(name), 0, 0, 0, {}, KnowledgeOrigin::Reported});
    return true;
}

bool PrivateKnowledge::Visit(uint32_t area, std::string name, uint64_t now, bool usefulWork)
{
    if (!area || name.empty() || !IsBoundedText(name, 100) || (!_places.contains(area) && _places.size() >= 64))
        return false;
    auto [found, inserted] = _places.try_emplace(area);
    auto& place = found->second;
    if (inserted)
    {
        place.area = area;
        place.name = std::move(name);
        place.origin = KnowledgeOrigin::Experienced;
        // Visiting alone does not establish a reliable level band for a new area.
        place.minimumLevel = 0;
        place.maximumLevel = 0;
    }
    place.visitedMs = std::max(place.visitedMs, now);
    if (usefulWork)
        place.lastUsefulWorkMs = std::max(place.lastUsefulWorkMs, now);
    return true;
}

bool PrivateKnowledge::RecordUsefulWork(uint32_t area, uint64_t now, uint8_t level)
{
    auto found = _places.find(area);
    if (found == _places.end() || !found->second.visitedMs || now < found->second.visitedMs || level > 80)
        return false;
    auto& place = found->second;
    place.lastUsefulWorkMs = std::max(place.lastUsefulWorkMs, now);
    if (level && place.origin != KnowledgeOrigin::Starting)
    {
        place.minimumLevel = place.minimumLevel ? std::min(place.minimumLevel, level) : level;
        place.maximumLevel = std::max(place.maximumLevel, level);
    }
    return true;
}

bool PrivateKnowledge::Investigate(uint32_t area)
{
    auto found = _places.find(area);
    if (found == _places.end() || found->second.investigations == std::numeric_limits<uint32_t>::max())
        return false;
    ++found->second.investigations;
    return true;
}

bool PrivateKnowledge::RememberRepair(uint32_t area, RepairLocation location)
{
    auto found = _places.find(area);
    if (found == _places.end() || !found->second.visitedMs || location.observedMs < found->second.visitedMs
        || !ValidRepairLocation(location))
        return false;
    auto& repair = found->second.repair;
    if (repair && (location.observedMs <= repair->observedMs || (repair->map == location.map
        && repair->phase == location.phase && DistanceSquared(*repair, location.x, location.y, location.z) < 100)))
        return false; // Remaining beside the same repairer does not dirty the owner's snapshot every tick.
    repair = location;
    return true;
}

KnownPlace const* PrivateKnowledge::NearestRepair(uint32_t map, uint32_t phase, float x, float y, float z,
    uint64_t now) const
{
    KnownPlace const* result = nullptr;
    float nearest = 600.0f * 600.0f; // The preparation's existing two-minute deadline bounds this local return trip.
    if (!phase || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return nullptr;
    for (auto const& [area, place] : _places)
        if (place.repair && place.repair->map == map && (place.repair->phase & phase)
            && place.repair->observedMs <= now)
        {
            float const distance = DistanceSquared(*place.repair, x, y, z);
            if (distance < nearest)
            {
                nearest = distance;
                result = &place;
            }
        }
    return result;
}

std::optional<uint64_t> PrivateKnowledge::Hear(Reference source, Association topic, std::string text,
    uint64_t now, double confidence)
{
    if ((!source.actor && source.name.empty()) || (source.actor && !IsValidActor(*source.actor))
        || !IsBoundedText(source.name, 100) || text.empty() || !IsBoundedText(text, 512)
        || topic.activity > Activity::Travel || (topic.person && !IsValidActor(*topic.person))
        || !std::isfinite(confidence) || confidence < 0 || confidence > 0.6
        || _nextReport == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    for (auto const& [id, report] : _reports)
        if (report.source == source && report.topic.activity == topic.activity && report.topic.quest == topic.quest
            && report.topic.place == topic.place && report.topic.person == topic.person && report.text == text)
            return id; // Repetition never renews the report's age or increases confidence.
    if (_reports.size() >= 64)
    {
        auto oldest = std::min_element(_reports.begin(), _reports.end(), [](auto const& left, auto const& right)
        {
            return left.second.receivedMs < right.second.receivedMs;
        });
        _reports.erase(oldest);
    }
    uint64_t const id = _nextReport++;
    _reports.emplace(id, LearnedReport{id, std::move(source), topic, std::move(text), now, confidence});
    return id;
}

bool PrivateKnowledge::Assess(uint64_t id, uint32_t visitedArea, bool usefulWork)
{
    auto found = _reports.find(id);
    auto place = _places.find(visitedArea);
    if (found == _reports.end() || place == _places.end() || !place->second.visitedMs
        || found->second.topic.place != visitedArea || place->second.visitedMs < found->second.receivedMs
        || place->second.visitedMs <= found->second.lastAssessedVisitMs
        || (usefulWork && place->second.lastUsefulWorkMs < place->second.visitedMs))
        return false;
    auto& count = usefulWork ? found->second.usefulVisits : found->second.unsuccessfulVisits;
    if (count == std::numeric_limits<uint32_t>::max())
        return false;
    ++count;
    found->second.lastAssessedVisitMs = place->second.visitedMs;
    // This records our follow-through separately; it does not rewrite what the speaker said or their certainty.
    return true;
}

std::vector<LearnedReport const*> PrivateKnowledge::Retrieve(Association const& topic,
    uint64_t now, std::size_t limit) const
{
    std::vector<LearnedReport const*> result;
    for (auto const& [id, report] : _reports)
        if (report.receivedMs <= now && Relevance(topic, report.topic))
            result.push_back(&report);
    std::sort(result.begin(), result.end(), [&topic](auto const* left, auto const* right)
    {
        auto const leftScore = Relevance(topic, left->topic);
        auto const rightScore = Relevance(topic, right->topic);
        if (leftScore != rightScore)
            return leftScore > rightScore;
        return left->receivedMs > right->receivedMs
            || (left->receivedMs == right->receivedMs && left->id < right->id);
    });
    result.resize(std::min({result.size(), limit, std::size_t(8)}));
    return result;
}

std::vector<KnownPlace const*> PrivateKnowledge::Alternatives(uint32_t currentArea, uint8_t level) const
{
    std::vector<KnownPlace const*> result;
    for (auto const& [area, place] : _places)
        if (area != currentArea && place.minimumLevel && place.minimumLevel <= level + 2
            && place.maximumLevel >= level)
            result.push_back(&place);
    // This is a set of suitable choices, not a route or a fixed zone progression.
    return result;
}
}
