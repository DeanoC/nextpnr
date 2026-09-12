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
 *  GPU routing backend interface.
 *
 *  This header is deliberately free of nextpnr types so that the device code
 *  (HIP for ROCm, or CUDA) can be compiled by the vendor compiler without
 *  pulling in the nextpnr headers. The host side of the router
 *  (common/route/gpurouter.cc) flattens the Arch routing graph into the plain
 *  arrays described by GraphData, and describes each batch of work as a list
 *  of tasks (one per net) with a list of arcs (one per sink) each.
 */

#ifndef GPUROUTE_BACKEND_H
#define GPUROUTE_BACKEND_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpuroute {

// Flattened routing graph in CSR (compressed sparse row) form.
//
// Wires are numbered 0..n_wires-1. Edge e (a pip) goes from the wire whose
// out_off range contains e to out_dst[e]. edge_cost[e] is the base cost of
// using the pip and the wire it drives, in nanoseconds:
//     >= 0     : usable by every net
//     <  0     : blocked for every net (architecture rule)
struct GraphData
{
    int32_t n_wires = 0;
    int64_t n_edges = 0;
    const int32_t *out_off = nullptr;   // n_wires + 1
    const int32_t *out_dst = nullptr;   // n_edges
    const float *edge_cost = nullptr;   // n_edges
    const int16_t *wire_x = nullptr;    // n_wires, notional location
    const int16_t *wire_y = nullptr;    // n_wires
    const int32_t *wire_reserved = nullptr; // n_wires, -1 or the only net allowed to use the wire
    const uint8_t *wire_flags = nullptr;    // n_wires, see WIRE_* below
    const int32_t *wire_occ = nullptr;      // n_wires, initial occupancy
    const float *wire_hist = nullptr;       // n_wires, initial history cost (1.0 = none)
};

enum WireFlags : uint8_t
{
    WIRE_UNAVAILABLE = 0x01, // locked to another net; never expand into it
};

// wire_reserved entries are -1 (free) or a net index, optionally with
// RESERVED_SOFT set: a soft reservation belongs to a frozen timing-repaired
// arc and may be overridden by a repair search with ignore_soft set.
constexpr int32_t RESERVED_SOFT = 0x40000000;
inline int32_t reserved_owner(int32_t r) { return r == -1 ? -1 : (r & ~RESERVED_SOFT); }

// Parameters of the cost function; identical for every task in a batch.
struct RouteParams
{
    float est_x = 0.075f;  // A* estimate, ns per tile in x
    float est_y = 0.2f;    // A* estimate, ns per tile in y
    float est_weight = 1.25f;
    float curr_cong_weight = 0.5f;
    float bias_factor = 0.25f;
    float seed_delay_weight = 1.0f; // tree wires start at (floor + (1-floor)*crit) * weight * upstream delay
    float seed_delay_floor = 0.0f;
    float load_penalty = 0.0f;   // ns added when branching off a tree wire, per branch it already drives
    float pip_adder = 0.0f;      // ns added to every pip's base cost
    int expand_k = 256;    // minimum frontier entries expanded per step
    int expand_div = 0;    // if > 0, also expand at least frontier_size / expand_div entries
    int use_bb = 1;        // honour task bounding boxes
    int ignore_soft = 0;   // treat other nets' soft reservations as free (repair displacement)
    int max_probe = 512;   // hash table probe limit
};

// One net to route. Arcs [arc_off, arc_off + arc_cnt) are routed in order;
// each arc is seeded from seeds[tree_off, tree_off + tree_cnt) plus the wires
// used by the arcs of this task routed earlier in the batch. A seed starts
// with cost crit * seed_delay_weight * upstream delay, so critical sinks
// prefer short total paths and non-critical sinks prefer sharing.
struct TaskDesc
{
    int32_t net = 0;
    int32_t tree_off = 0, tree_cnt = 0;
    int32_t arc_off = 0, arc_cnt = 0;
    int32_t path_off = 0, path_cap = 0; // output region (in pairs) for this task
    int32_t cx = 0, cy = 0, hpwl = 1, fanout = 1;
    int16_t bb_x0 = 0, bb_y0 = 0, bb_x1 = 0, bb_y1 = 0;
};

struct ArcDesc
{
    int32_t sink = 0;
    float crit_weight = 1.0f; // max(0.05, 1 - crit^2): scales congestion terms
    float crit = 0.0f;        // timing criticality in [0, 1]
};

enum ArcStatus : int32_t
{
    ARC_OK = 0,
    ARC_NO_PATH = 1,   // search space exhausted inside the bounding box
    ARC_OVERFLOW = 2,  // hash table or queue too small; retry in the large lane
    ARC_PATH_FULL = 3, // path output region too small
};

enum FailReason : int32_t
{
    FAIL_NONE = 0,
    FAIL_PROBE = 1,     // hash probe limit hit
    FAIL_LOAD = 2,      // hash table load factor exceeded
    FAIL_PILE = 3,      // frontier pile capacity exceeded
};

struct ArcResult
{
    int32_t status = ARC_NO_PATH;
    int32_t path_off = 0; // absolute index (in pairs) of the first path entry
    int32_t path_len = 0; // number of (wire, parent) pairs, sink first
    int32_t expanded = 0; // wires popped from the frontier
    int32_t steps = 0;    // parallel expansion steps
    int32_t reason = 0;   // FailReason for ARC_OVERFLOW
    float cost = 0.0f;    // accumulated cost of the found path
};

// Path entries are listed from the sink back to (but excluding) the first
// wire that was already part of the net's tree. edge is the CSR index of the
// pip parent -> wire that the search selected (architectures may have
// several pips between the same pair of wires); delay is the base delay
// from the net source to wire along the tree (ns).
struct PathEntry
{
    int32_t wire;
    int32_t parent;
    int32_t edge;
    float delay;
};

struct BackendStats
{
    double route_seconds = 0.0;   // time spent in route()
    double transfer_seconds = 0.0;
    int64_t launches = 0;
    int64_t arcs_routed = 0;
    int64_t wires_expanded = 0;
};

class Backend
{
  public:
    virtual ~Backend() = default;

    // Upload the graph and allocate scratch for the two lanes.
    // The 'small' lane uses many concurrent tasks with 2^small_bits entries
    // per hash table, the 'large' lane fewer tasks with 2^large_bits entries.
    virtual bool init(const GraphData &graph, int small_slots, int small_bits, int large_slots, int large_bits,
                      std::string &error) = 0;

    // Sparse update of occupancy, history and reservation for the listed wires.
    virtual void update_wire_state(size_t count, const int32_t *wires, const int32_t *occ, const float *hist,
                                   const int32_t *reserved) = 0;

    // Route a batch of tasks. results has one entry per arc (in ArcDesc
    // order); paths receives the concatenated path entries. seed_delay[i]
    // is the base delay from the net source to seeds[i] along the tree and
    // seed_load[i] the number of tree branches seeds[i] already drives.
    virtual void route(const RouteParams &params, const std::vector<TaskDesc> &tasks,
                       const std::vector<ArcDesc> &arcs, const std::vector<int32_t> &seeds,
                       const std::vector<float> &seed_delay, const std::vector<float> &seed_load, bool large_lane,
                       std::vector<ArcResult> &results, std::vector<PathEntry> &paths) = 0;

    virtual const char *name() const = 0;
    virtual const BackendStats &stats() const = 0;
};

// True if this build contains a GPU device backend.
bool device_backend_available();
// Human readable description of the GPU devices (empty if none).
std::string describe_devices();

// Create the device backend. device < 0 selects the device with the most
// compute units. Returns nullptr and sets error on failure.
std::unique_ptr<Backend> create_device_backend(int device, std::string &error);

// Sequential host reference implementation of the same algorithm.
std::unique_ptr<Backend> create_cpu_backend();

} // namespace gpuroute

#endif // GPUROUTE_BACKEND_H
