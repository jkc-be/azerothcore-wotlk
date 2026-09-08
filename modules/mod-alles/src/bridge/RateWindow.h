/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 * Released under GNU GPL v2 or later; see COPYING.
 */
#ifndef MOD_ALLES_RATE_WINDOW_H
#define MOD_ALLES_RATE_WINDOW_H

#include <cstdint>
#include <map>
#include <stdexcept>

namespace Alles::Bridge
{
// Real epoch buckets round admission up by at most 999 ms. This never grants an early replenishment.
// Keep 105 seconds for recovery: a reserved job can start its call up to 45 seconds later.
// Counts, rather than one timestamp per call, keep Unlimited mode's accounting bounded at high throughput.
using ReservationHistory = std::map<uint64_t, uint64_t>;
inline void PruneReservations(ReservationHistory& history, uint64_t now)
{
    std::erase_if(history, [now](auto const& item) { return now >= item.first && now - item.first >= 105000; });
}
inline void AddReservation(ReservationHistory& history, uint64_t now, uint64_t count = 1)
{
    PruneReservations(history, now);
    auto const bucket = now / 1000 * 1000 + (now % 1000 ? 1000 : 0);
    if (!history.contains(bucket) && history.size() >= 128)
        throw std::runtime_error("reservation clock moved outside bounded history");
    history[bucket] += count;
}
inline uint64_t RecentCount(ReservationHistory const& history, uint64_t now)
{
    uint64_t count = 0;
    for (auto const& [time, calls] : history)
        if (now < time || now - time < 60000)
            count += calls;
    return count;
}
}
#endif
