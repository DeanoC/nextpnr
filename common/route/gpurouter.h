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

#include <memory>
#include <string>
#include <vector>

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct GpuRouterCfg
{
    GpuRouterCfg(Context *ctx);

    // Padding added to net bounding boxes (tiles)
    int bb_margin_x, bb_margin_y;
    // Congestion cost schedule (same meaning as router2)
    float init_curr_cong_weight, hist_cong_weight, curr_cong_mult;
    // If the negotiated-congestion loop's overused-wire count sets no new
    // minimum for this many iterations, the present-congestion weight grows
    // congestion_stall_boost times faster until it does. A handful of nets
    // stuck swapping the same local wire (e.g. two cells placed with a
    // shared LAB-internal conflict) can otherwise cycle for a very long
    // time under the plain additive schedule; this only ever accelerates
    // convergence and never gives up on its own (max_iter still bounds the
    // loop).
    int congestion_stall_iters;
    float congestion_stall_boost;
    // Ceiling on the compounded boost multiplier. A genuine hard conflict
    // (a net with no legal alternative route at all, e.g. it is competing
    // with a reserved/frozen wire) does not respond to any amount of
    // present-congestion cost, so letting the multiplier compound without
    // bound just produces absurd weights and log noise; max_iter is the
    // real backstop for that case.
    float congestion_stall_boost_max;
    // Weight of the A* estimate; > 1 trades optimality for speed
    float estimate_weight;
    // The same for the pure-delay searches of timing repair and candidate
    // generation, which are few and are what critical arcs end up with;
    // defaults to estimate_weight
    float repair_estimate_weight;
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
    // When a later same-band repair is blocked by an already-frozen peer,
    // those nets are ripped together and re-routed at pure delay plus this
    // present-congestion weight so they share short wires instead of the
    // first freeze leaving the rest unroutable. 1.0 makes an occupied wire
    // cost 2x delay. 0 disables the peer-group pass.
    float repair_cong_weight;
    // Batches of at most this many nets run on the host backend (0 = never)
    int cpu_lane_nets;
    // Candidate generation (GpuCandidateRouter): tiles added to the net's
    // bounding box, and whether the two primary variants may fall back to
    // the search without a box
    int candidate_margin;
    bool candidate_unbounded;
    // Frontier entries expanded per step in candidate searches (default
    // expand_k): they run one net at a time, so a larger step means fewer
    // sequential steps at some cost in path quality
    int candidate_expand_k;
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

// An alternative routing tree for one net produced by GpuCandidateRouter:
// every wire the net uses with the pip that drives it (PipId() for the
// source and for wires bound without a pip).
struct GpuRouteTree
{
    std::vector<std::pair<WireId, PipId>> wires;
    int variant = 0;         // diversity rule that produced it (see candidates())
    delay_t route_delay = 0; // delay-table route delay of the re-routed sink
};

// Produces several materially different pure-delay routes for one sink of an
// already-routed net while every other net stays where the Arch has bound
// it, so that an architecture with a more accurate delay model than the
// per-pip table (Mistral's analogue model) can choose between them instead
// of accepting the single route the scalar search prefers. The routing graph
// is flattened from the current Arch bindings when the object is created;
// resync() reloads one net after the caller has rebound it.
class GpuCandidateRouter
{
  public:
    GpuCandidateRouter(Context *ctx, const GpuRouterCfg &cfg);
    ~GpuCandidateRouter();
    struct Sink
    {
        NetInfo *net;
        store_index<PortRef> user;
    };
    // For each sink, up to `count` distinct trees that re-route it at pure
    // delay, in this order: 0 attaches anywhere on the existing tree (the
    // router's own choice), 1 routes from the source only, then ones
    // avoiding the multi-tile wires of every earlier candidate while that
    // still finds new routes, then one that avoids each multi-tile wire of
    // the route the arc has now, and, while still below `count`, for a net
    // with several sinks whole-tree candidates (variant 100: every sink
    // rebuilt, the failing one first; 101: a star, every sink from the
    // source). The sinks are searched
    // together, one variant per launch; a second sink of the same net gets
    // no candidates in this call. The route the net already has and repeats are left
    // out. The nets' routing in the Arch is left unchanged; candidates of
    // different nets may compete for the same free wire, which the caller
    // sees when it binds them.
    std::vector<std::vector<GpuRouteTree>> candidates(const std::vector<Sink> &sinks, int count);
    // The caller rebound `net` in the Arch; reload its tree from there.
    void resync(NetInfo *net);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

NEXTPNR_NAMESPACE_END

#endif // GPUROUTER_H
