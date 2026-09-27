/*
 * Copyright (C) 2026 Deano Calver
 * SPDX-License-Identifier: MIT
 */

#ifndef GPUROUTE_CONGESTION_PLATEAU_H
#define GPUROUTE_CONGESTION_PLATEAU_H

namespace gpuroute {

// Early termination for negotiation attempts that started with frozen
// timing-repair arcs. Ordinary initial routing retains its iteration budget.
class CongestionPlateau
{
    bool repair_attempt = false;
    int tiny_overuse_iters = 0;

  public:
    void begin(bool has_frozen_arcs)
    {
        repair_attempt = has_frozen_arcs;
        clear_tiny();
    }

    void clear_tiny() { tiny_overuse_iters = 0; }

    bool saturated(int overused_wires, int failed_nets) const
    {
        return repair_attempt && overused_wires > 0 && overused_wires <= 8 && failed_nets <= 8;
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
