/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef GPUROUTE_RESERVED_ROUTE_SLACK_H
#define GPUROUTE_RESERVED_ROUTE_SLACK_H

#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace gpuroute {

// One physical route into a sink port. Setup slack is the value from the last
// timing update, which still describes `original`. `incumbent` is the
// pure-delay route already installed; `alternate` walks other nets' soft
// reservations. Several physical pins of one port share that slack, and the
// timing graph stores the maximum of their delays.
template <typename Delay> struct ReservedRouteArc
{
    int port = 0;
    Delay original{};
    Delay incumbent{};
    Delay alternate{};
    float slack = 0.f;
    bool searched = false;
};

template <typename Delay> struct ReservedRouteVerdict
{
    bool accept = false;
    float incumbent_worst = std::numeric_limits<float>::infinity();
    float alternate_worst = std::numeric_limits<float>::infinity();
    Delay incumbent_sum{};
    Delay alternate_sum{};
};

// Keep the alternate when its worst setup slack does not fall. A tie keeps
// the incumbent unless the searched arcs' delay sum is lower. Both slacks are
// estimated from the same pre-repair baseline, so a pure-delay improvement
// that timing has not refreshed yet is not charged to the alternate.
template <typename Delay>
ReservedRouteVerdict<Delay> judge_reserved_route(const std::vector<ReservedRouteArc<Delay>> &arcs)
{
    ReservedRouteVerdict<Delay> verdict;
    if (arcs.empty())
        return verdict;

    struct Port
    {
        Delay original_max{};
        Delay incumbent_max{};
        Delay alternate_max{};
        float slack = 0.f;
        bool seen = false;
    };
    std::map<int, Port> ports;
    for (const auto &arc : arcs) {
        Port &port = ports[arc.port];
        if (!port.seen) {
            port.seen = true;
            port.original_max = arc.original;
            port.incumbent_max = arc.incumbent;
            port.alternate_max = arc.alternate;
            port.slack = arc.slack;
        } else {
            port.original_max = std::max(port.original_max, arc.original);
            port.incumbent_max = std::max(port.incumbent_max, arc.incumbent);
            port.alternate_max = std::max(port.alternate_max, arc.alternate);
        }
        if (arc.searched) {
            verdict.incumbent_sum += arc.incumbent;
            verdict.alternate_sum += arc.alternate;
        }
    }

    for (const auto &entry : ports) {
        const Port &port = entry.second;
        verdict.incumbent_worst = std::min(verdict.incumbent_worst,
                                           port.slack - static_cast<float>(port.incumbent_max - port.original_max));
        verdict.alternate_worst = std::min(verdict.alternate_worst,
                                           port.slack - static_cast<float>(port.alternate_max - port.original_max));
    }

    // Same 1 ps band the repair round uses when it decides the worst slack moved.
    const bool slack_worse = verdict.alternate_worst < verdict.incumbent_worst - 1.0f;
    const bool slack_better = verdict.alternate_worst > verdict.incumbent_worst + 1.0f;
    verdict.accept = !slack_worse && (slack_better || verdict.alternate_sum < verdict.incumbent_sum);
    return verdict;
}

} // namespace gpuroute

#endif
