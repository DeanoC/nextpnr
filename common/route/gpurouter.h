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
 *
 *  GPU-accelerated connection-based negotiated-congestion router.
 *  See docs/gpurouter.md for the design.
 */

#ifndef GPUROUTER_H
#define GPUROUTER_H

#include <string>

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct GpuRouterCfg
{
    GpuRouterCfg(Context *ctx);

    // Padding added to net bounding boxes (tiles)
    int bb_margin_x, bb_margin_y;
    // Congestion cost schedule (same meaning as router2)
    float init_curr_cong_weight, hist_cong_weight, curr_cong_mult;
    // Weight of the A* estimate; > 1 trades optimality for speed
    float estimate_weight;
    // Bias towards the net centroid, as a fraction of the base cost
    float bias_cost_factor;
    // A sink may attach anywhere on the net's existing tree; the attach
    // point starts with cost crit * seed_delay_weight * upstream delay
    float seed_delay_weight, seed_delay_floor;
    // Lower bound of the criticality weight max(floor, 1 - crit^2) that
    // scales the congestion terms; router2 uses 0.05
    float crit_weight_floor;
    // 0: weight = 1 - crit^2 (router2); 1: weight = (1 - crit)^crit_exponent
    int crit_weight_mode;
    float crit_exponent;
    // Analogue-model approximations: ns added when branching off a tree
    // wire per branch it already drives, and ns added to every pip
    float load_penalty, pip_adder;
    // Iterations without fewer slack-failing arcs before timing-driven
    // rip-up (--tmg-ripup) gives up
    int tmg_ripup_patience;
    // After convergence, arcs with setup slack below repair_slack (ps) are
    // re-routed at pure delay and frozen, for up to repair_rounds rounds
    int repair_rounds;
    float repair_slack;
    // While nothing fails, arcs within repair_band (ps) of the worst slack
    // are still repaired, until the worst slack stops improving
    float repair_band;
    int repair_improve_rounds;
    // A repair that cannot find its minimum-delay route may displace frozen
    // arcs of other nets with at least repair_displace_margin (ps) more slack
    bool repair_displace;
    float repair_displace_margin;
    // Batches of at most this many nets run on the host backend (0 = never)
    int cpu_lane_nets;
    // Frontier entries expanded per parallel step: at least expand_k and at
    // least frontier_size / expand_div (expand_k = 1, expand_div = 0 is A*)
    int expand_k, expand_div;
    // Bounding-box-disjoint batches per iteration before nets are allowed
    // to be routed concurrently with overlapping boxes
    int max_batches;
    // Maximum number of nets routed concurrently on the device
    int small_slots, large_slots;
    // log2 of the per-net hash table size for the two lanes
    int small_bits, large_bits;
    // GPU device index, -1 selects automatically
    int device;
    // Use the sequential host reference backend instead of the GPU
    bool cpu_backend;
    // Print per-iteration timing and backend statistics
    bool perf_profile;
    // Stop after this many iterations without convergence
    int max_iter;
};

// Returns true when the whole design routed (and, as with router2, after the
// result has been checked by router1).
bool gpurouter(Context *ctx, const GpuRouterCfg &cfg);

NEXTPNR_NAMESPACE_END

#endif // GPUROUTER_H
