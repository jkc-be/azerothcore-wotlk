/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#ifndef MOD_ALLES_EXPLORATION_H
#define MOD_ALLES_EXPLORATION_H

#include <cstdint>
#include <utility>
#include <vector>

namespace Alles
{
// Transient evidence of actual local searches. Reload starts a fresh survey, never assumed offline investigation.
class LocalSurvey
{
public:
    void Observe(uint64_t now, uint64_t scan, bool empty, bool active, float x, float y);
    bool Exhausted() const { return _activeMs >= 120000 && _positions.size() >= 3 && _emptyScans >= 3; }
    uint64_t ActiveMs() const { return _activeMs; }
    uint32_t EmptyScans() const { return _emptyScans; }
    std::size_t Positions() const { return _positions.size(); }

private:
    uint64_t _lastSampleMs = 0;
    uint64_t _lastScanMs = 0;
    uint64_t _scan = 0;
    uint64_t _activeMs = 0;
    uint32_t _emptyScans = 0;
    bool _active = false;
    std::vector<std::pair<float, float>> _positions;
};
}
#endif
