/* Analogue repair reroute error policy and per-clock timing guard. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_REPAIR_H
#define MISTRAL_ANALOGUE_REPAIR_H

#include <utility>
#include <cmath>
#include <limits>
#include <vector>

#include "gpurouter.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

// Local router extends an empty net or a rooted partial tree, preserving its parents.
// Consumes a shared node budget and leaves partial routing on failure.
bool critical_local_route_net(Context *ctx, NetInfo *net, int &visits);
class TimingAnalyser;
// Every structural source must match the complete native launch inventory.
bool critical_lut6_swap_source_coverage(Context *ctx, TimingAnalyser &timing, CellInfo *target,
                                        std::string *reason = nullptr);
// Separate ordinary ALUT2..5 scope; does not widen the LUT6 admission boundary.
bool critical_lut_permutation_source_coverage(Context *ctx, TimingAnalyser &timing, CellInfo *target,
                                             std::string *reason = nullptr);
// Fixed-BEL logical transposition with native pin allocation and shared-tree rollback.
bool critical_routed_lut_permutation_trial(Context *ctx, CellInfo *target, IdString first, IdString second,
    int visit_budget, const std::function<bool()> &accept, std::string *reason = nullptr,
    const std::function<void(unsigned)> &stage = {}, bool keep_source_exits = false);
// Every affected output branch needs complete native setup and hold coverage.
bool critical_lut6_swap_output_coverage(Context *ctx, TimingAnalyser &timing, NetInfo *output,
                                         std::string *reason = nullptr);
// Explicit optional coverage for proven unobserved SO/CO leaves; never an empty root.
bool critical_output_coverage_with_unobserved_arithmetic(Context *ctx, TimingAnalyser &timing, NetInfo *output,
                                                        std::string *reason = nullptr);
// Symmetric single-sink LUT6 input swap with atomic two-net rollback.
bool critical_routed_lut6_swap_trial(Context *ctx, CellInfo *cell, IdString first, IdString second, int visit_budget,
                                    const std::function<bool()> &accept, std::string *reason = nullptr,
                                    const std::function<void(unsigned)> &stage = {});
// Empty-site LUT move: restore placement, incident routes and physical state together.
bool critical_routed_move_trial(Context *ctx, CellInfo *cell, BelId target, int visit_budget,
                                const std::function<bool()> &accept, std::string *reason = nullptr);

// Restore the net, physical mux state and analogue cache on rejection or exception.
bool critical_route_trial(Context *ctx, NetInfo *net, const GpuRouteTree &candidate,
                          const std::function<bool()> &accept);

// Ranking only: native arc delay includes the terminal LUT input stage.
// Full-design timing and hold checks remain mandatory before retention.
struct CriticalRouteSinkDelay { float slack, before, after; };
inline float critical_route_local_gain(const std::vector<CriticalRouteSinkDelay> &sinks)
{
    float before = std::numeric_limits<float>::infinity();
    float after = before;
    for (const auto &sink : sinks) {
        if (!std::isfinite(sink.slack) || !std::isfinite(sink.before) || !std::isfinite(sink.after) ||
            sink.slack == std::numeric_limits<float>::lowest() ||
            sink.slack == std::numeric_limits<float>::max())
            return -std::numeric_limits<float>::infinity();
        before = std::min(before, sink.slack);
        after = std::min(after, sink.slack + sink.before - sink.after);
    }
    if (sinks.empty() || before >= 0) return -std::numeric_limits<float>::infinity();
    return after - before;
}

template <typename Route> bool try_analogue_repair_reroute(int round, Route &&route)
{
    try {
        return std::forward<Route>(route)();
    } catch (const GpuRequiredFailure &) {
        throw;
    } catch (const log_execution_error_exception &) {
        log_warning("analogue repair round %d could not route; keeping the best earlier round\n", round);
        return false;
    }
}

// Setup slack, hold slack and Fmax of every clock of one timing analysis
// (with the timing report built, as for signoff).
struct AnalogueClockTiming
{
    dict<IdString, delay_t> setup, hold;
    dict<IdString, float> fmax;

    AnalogueClockTiming() = default;
    explicit AnalogueClockTiming(const TimingResult &result)
            : setup(result.clock_setup_slack), hold(result.clock_hold_slack)
    {
        for (const auto &clock : result.clock_fmax)
            fmax[clock.first] = clock.second.achieved;
    }

    // Whether this timing gives a clock of `before` less setup slack or a
    // lower Fmax, or less hold slack where that is now zero or below: the
    // hold check fails at zero, so a positive hold slack may shrink but must
    // stay positive, and one that was not may not shrink at all. A clock that
    // is no longer timed counts as worse.
    bool worse_than(const AnalogueClockTiming &before) const
    {
        for (const auto &clock : before.setup) {
            auto now = setup.find(clock.first);
            if (now == setup.end() || now->second < clock.second)
                return true;
        }
        for (const auto &clock : before.fmax) {
            auto now = fmax.find(clock.first);
            if (now == fmax.end() || now->second < clock.second)
                return true;
        }
        for (const auto &clock : before.hold) {
            auto now = hold.find(clock.first);
            if (now == hold.end() || (now->second < clock.second && now->second <= 0))
                return true;
        }
        return false;
    }

    // Whether this timing gives any clock of `before` more setup slack or a
    // higher Fmax.
    bool better_than(const AnalogueClockTiming &before) const
    {
        for (const auto &clock : before.setup) {
            auto now = setup.find(clock.first);
            if (now != setup.end() && now->second > clock.second)
                return true;
        }
        for (const auto &clock : before.fmax) {
            auto now = fmax.find(clock.first);
            if (now != fmax.end() && now->second > clock.second)
                return true;
        }
        return false;
    }
};

NEXTPNR_NAMESPACE_END

#endif
