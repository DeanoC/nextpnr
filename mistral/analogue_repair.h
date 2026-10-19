/* Analogue repair reroute error policy and per-clock timing guard. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_REPAIR_H
#define MISTRAL_ANALOGUE_REPAIR_H

#include <utility>

#include "gpurouter.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

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
