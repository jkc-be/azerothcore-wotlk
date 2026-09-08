/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_KNOWLEDGE_H
#define MOD_ALLES_KNOWLEDGE_H

#include "Memory.h"
#include <map>
#include <vector>

namespace Alles
{
enum class KnowledgeOrigin : uint8_t
{
    Starting, Experienced, Reported
};

enum class Activity : uint8_t
{
    Work, Hunt, Supplies, Companions, Travel
};

struct Association
{
    Activity activity = Activity::Work;
    uint32_t quest = 0;
    uint32_t place = 0;
    std::optional<ActorKey> person;

    bool operator==(Association const&) const = default;
};

// A personally observed place to look again, never a live NPC handle or authority to repair remotely.
struct RepairLocation
{
    uint32_t map = 0;
    uint32_t phase = 0;
    float x = 0, y = 0, z = 0;
    uint64_t observedMs = 0;

    bool operator==(RepairLocation const&) const = default;
};

struct KnownPlace
{
    uint32_t area = 0;
    std::string name;
    uint8_t minimumLevel = 1;
    uint8_t maximumLevel = 10;
    uint32_t relativeTo = 0;
    std::string direction;
    KnowledgeOrigin origin = KnowledgeOrigin::Starting;
    uint64_t visitedMs = 0;
    uint64_t lastUsefulWorkMs = 0;
    uint32_t investigations = 0;
    std::optional<RepairLocation> repair = std::nullopt;
    // A report remains a separate source-attributed record even after this place is visited.
    bool operator==(KnownPlace const&) const = default;
};

struct LearnedReport
{
    uint64_t id = 0;
    Reference source;
    Association topic;
    std::string text;
    uint64_t receivedMs = 0;
    double confidence = 0;
    uint32_t usefulVisits = 0;
    uint32_t unsuccessfulVisits = 0;
    uint64_t lastAssessedVisitMs = 0;

    bool operator==(LearnedReport const&) const = default;
};

struct KnowledgeSnapshot
{
    uint32_t seedVersion = 0;
    uint64_t nextReport = 1;
    std::map<uint32_t, KnownPlace> places;
    std::map<uint64_t, LearnedReport> reports;

    bool operator==(KnowledgeSnapshot const&) const = default;
};

bool IsValidKnowledgeSnapshot(KnowledgeSnapshot const& snapshot);

struct StartingProfile
{
    uint32_t version = 1;
    std::string name;
    std::vector<KnownPlace> places;
};

// Shared templates contain geography only. No owner state is stored in these templates.
std::optional<StartingProfile> StartingGeography(uint8_t race);

// An instance belongs to exactly one ActorStore owner. It has no cross-owner lookup or global live cache.
class PrivateKnowledge
{
public:
    bool Seed(uint8_t race, bool deathKnight, bool startingExperienceComplete);
    bool LearnPlace(uint32_t area, std::string name);
    bool Visit(uint32_t area, std::string name, uint64_t now, bool usefulWork);
    bool RecordUsefulWork(uint32_t area, uint64_t now, uint8_t level = 0);
    bool Investigate(uint32_t area);
    bool RememberRepair(uint32_t area, RepairLocation location);
    KnownPlace const* NearestRepair(uint32_t map, uint32_t phase, float x, float y, float z, uint64_t now) const;
    std::optional<uint64_t> Hear(Reference source, Association topic, std::string text,
        uint64_t now, double confidence);
    bool Assess(uint64_t report, uint32_t visitedArea, bool usefulWork);
    std::vector<LearnedReport const*> Retrieve(Association const& topic, uint64_t now, std::size_t limit = 8) const;
    std::vector<KnownPlace const*> Alternatives(uint32_t currentArea, uint8_t level) const;
    std::map<uint32_t, KnownPlace> const& Places() const { return _places; }
    std::map<uint64_t, LearnedReport> const& Reports() const { return _reports; }
    uint32_t SeedVersion() const { return _seedVersion; }
    KnowledgeSnapshot Capture() const;
    bool Restore(KnowledgeSnapshot snapshot);

private:
    uint32_t _seedVersion = 0;
    uint64_t _nextReport = 1;
    std::map<uint32_t, KnownPlace> _places;
    std::map<uint64_t, LearnedReport> _reports;
};
}
#endif
