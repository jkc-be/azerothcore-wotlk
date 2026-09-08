/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */

#include "Exploration.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Alles
{
void LocalSurvey::Observe(uint64_t now, uint64_t scan, bool empty, bool active, float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y))
        active = false;
    if (scan < _scan || now < _lastSampleMs)
        *this = {};
    if (scan != _scan)
    {
        _scan = scan;
        _lastScanMs = now;
        if (!empty)
        {
            _activeMs = 0;
            _emptyScans = 0;
            _positions.clear();
        }
        else if (active)
        {
            if (_emptyScans < std::numeric_limits<uint32_t>::max())
                ++_emptyScans;
            if (_positions.size() < 8 && std::none_of(_positions.begin(), _positions.end(), [x, y](auto const& point)
                { return std::hypot(x - point.first, y - point.second) < 25.0f; }))
                _positions.emplace_back(x, y);
        }
    }
    active = active && empty && scan && now >= _lastScanMs && now - _lastScanMs <= 10000;
    if (active && _active && now >= _lastSampleMs && now - _lastSampleMs <= 2000)
        _activeMs += now - _lastSampleMs;
    _lastSampleMs = now;
    _active = active;
}
}
