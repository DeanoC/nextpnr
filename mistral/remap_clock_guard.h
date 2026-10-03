/* Complete endpoint clock-pair guards for optional remapping. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_REMAP_CLOCK_GUARD_H
#define MISTRAL_REMAP_CLOCK_GUARD_H
#include "nextpnr.h"
#include "timing.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

NEXTPNR_NAMESPACE_BEGIN
namespace mistral_remap_clock_guard {
using Rows = std::vector<EndpointClockPairTiming>;
inline bool timed(float slack)
{
    return std::isfinite(slack) && slack < float(std::numeric_limits<delay_t>::max());
}
inline bool rows_match(const Rows &before, const Rows &after)
{
    if (before.empty() || before.size() != after.size()) return false;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto &old = before[i], &now = after[i];
        if (!(old.launch == now.launch) || !(old.capture == now.capture) || old.setup_timed != now.setup_timed ||
            old.hold_related != now.hold_related || old.setup_window != now.setup_window ||
            old.setup_margin.has_value() != now.setup_margin.has_value() ||
            old.hold_margin.has_value() != now.hold_margin.has_value()) return false;
    }
    return true;
}
inline bool rows_nonregressing(const Rows &before, const Rows &after, bool reference_free)
{
    if (!rows_match(before, after)) return false;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto &old = before[i], &now = after[i];
        if (!old.setup_timed) {
            if (now.max_path_delay > old.max_path_delay || now.min_path_delay < old.min_path_delay) return false;
        } else if (!reference_free) {
            if (!old.setup_window || !old.setup_margin || now.max_path_delay > old.max_path_delay ||
                *now.setup_margin < *old.setup_margin) return false;
        }
        if (!reference_free && old.hold_related &&
            (!old.hold_margin || *now.hold_margin < std::min(delay_t(0), *old.hold_margin))) return false;
    }
    return true;
}
using HoldRows = std::map<std::string, int64_t>;
inline HoldRows holds(TimingAnalyser &timing)
{
    HoldRows result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int64_t value = 0;
        for (const auto &segment : path.segments) value += segment.delay;
        auto end = path.segments.back().to;
        auto key = std::to_string(path.clock_pair.start.clock.index) + ":" +
            std::to_string(int(path.clock_pair.start.edge)) + ":" + std::to_string(path.clock_pair.end.clock.index) + ":" +
            std::to_string(int(path.clock_pair.end.edge)) + ":" + std::to_string(end.first.index) + ":" + std::to_string(end.second.index);
        auto inserted = result.emplace(key, value);
        if (!inserted.second) inserted.first->second = std::min(inserted.first->second, value);
    }
    return result;
}
inline bool holds_nonregressing(const HoldRows &before, const HoldRows &after)
{
    for (const auto &entry : after) {
        auto old = before.find(entry.first);
        if (old == before.end() || entry.second < old->second) return false;
    }
    return true;
}
inline bool clocks_nonregressing(TimingAnalyser &before, TimingAnalyser &after)
{
    const auto &old = before.get_timing_result().clock_fmax, &now = after.get_timing_result().clock_fmax;
    if (old.empty() || old.size() != now.size()) return false;
    for (const auto &clock : old)
        if (!now.count(clock.first) || !std::isfinite(clock.second.achieved) || !std::isfinite(clock.second.constraint) ||
            !std::isfinite(now.at(clock.first).achieved) ||
            now.at(clock.first).constraint != clock.second.constraint ||
            now.at(clock.first).achieved + 1e-4 < clock.second.achieved) return false;
    return true;
}
} // namespace mistral_remap_clock_guard
NEXTPNR_NAMESPACE_END
#endif
