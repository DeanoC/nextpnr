/*
 * Copyright (C) 2026 Deano Calver
 * SPDX-License-Identifier: MIT
 */

#ifndef GPUROUTE_CONGESTION_PLATEAU_H
#define GPUROUTE_CONGESTION_PLATEAU_H

#include <limits>

namespace gpuroute {

// Early termination for negotiation attempts that started with frozen
// timing-repair arcs. Ordinary initial routing retains its iteration budget.
class CongestionPlateau
{
    bool repair_attempt = false;
    int tiny_overuse_iters = 0;
    int attempt_best_overused = std::numeric_limits<int>::max();
    int stalled_ceiling_periods = 0;

  public:
    static constexpr int ceiling_stall_periods = 3;

    void begin(bool has_frozen_arcs)
    {
        repair_attempt = has_frozen_arcs;
        clear_tiny();
        attempt_best_overused = std::numeric_limits<int>::max();
        stalled_ceiling_periods = 0;
    }

    void clear_tiny() { tiny_overuse_iters = 0; }

    void observe(int overused_wires)
    {
        if (overused_wires <= 0) {
            attempt_best_overused = std::numeric_limits<int>::max();
            stalled_ceiling_periods = 0;
            return;
        }
        if (overused_wires < attempt_best_overused) {
            attempt_best_overused = overused_wires;
            stalled_ceiling_periods = 0;
        }
    }

    bool saturated(int overused_wires, int failed_nets) const
    {
        return repair_attempt && overused_wires > 0 && overused_wires <= 8 && failed_nets <= 8;
    }

    // Checked at each congestion-stall period once present-congestion boost is
    // already at its ceiling. A repair attempt whose overuse stops falling
    // exits regardless of plateau size; initial routing keeps its full budget.
    bool stalled_at_ceiling(int overused_wires)
    {
        if (!repair_attempt || overused_wires <= 0) {
            stalled_ceiling_periods = 0;
            return false;
        }
        if (overused_wires < attempt_best_overused) {
            attempt_best_overused = overused_wires;
            stalled_ceiling_periods = 0;
            return false;
        }
        return ++stalled_ceiling_periods >= ceiling_stall_periods;
    }

    bool tiny(int overused_wires)
    {
        if (overused_wires > 0 && overused_wires <= 4)
            ++tiny_overuse_iters;
        else
            clear_tiny();
        return repair_attempt && tiny_overuse_iters >= 30;
    }
};

} // namespace gpuroute

#endif
