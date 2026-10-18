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
#include <utility>
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
    // n_edges. 1 if a non-member net must not use the pip. nullptr means no
    // edge is reserved, the same as an all-zero array. Backends honour that
    // default so a caller that does not care about reserved regions (including
    // hand-built test graphs) never has to allocate a dummy buffer.
    const uint8_t *edge_region = nullptr;
    // n_edges. 1 if a halo-constrained net may use the pip only when the
    // destination wire's tile is one of that task's shell tiles. nullptr
    // means no such pips, the same as an all-zero array.
    const uint8_t *edge_outside_halo = nullptr;
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

// Packed tile key. The low 16 bits are x, the high 16 bits are y, matching
// the int16 wire coordinates the graph stores.
#if defined(__CUDACC__) || defined(__HIPCC__)
#define GPUROUTE_HD __host__ __device__
#else
#define GPUROUTE_HD
#endif
GPUROUTE_HD inline int32_t pack_shell_tile(int x, int y)
{
    return int32_t(uint16_t(int16_t(x))) | (int32_t(uint16_t(int16_t(y))) << 16);
}
// True when (x, y) is one of tiles[off, off + cnt). A null list or a
// non-positive count matches nothing.
GPUROUTE_HD inline bool shell_tile_hit(const int32_t *tiles, int off, int cnt, int x, int y)
{
    if (cnt <= 0 || tiles == nullptr)
        return false;
    const int32_t key = pack_shell_tile(x, y);
    for (int i = 0; i < cnt; i++)
        if (tiles[off + i] == key)
            return true;
    return false;
}
#undef GPUROUTE_HD

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
    int ignore_hist = 0;   // drop history cost (peer-group present-congestion only)
    int max_probe = 512;   // hash table probe limit
    int exact = 0;         // CPU backend only: expand exactly the lowest-f entry per step (plain
                           // Dijkstra/A*, a reference for the K-best search); the device ignores it
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
    int32_t allow_reserved = 1; // 0: do not expand edge_region edges
    // 1: do not expand edge_outside_halo edges unless the destination tile is
    // one of shell_tiles[shell_off, shell_off + shell_cnt).
    int32_t stay_in_halo = 0;
    int32_t shell_off = 0, shell_cnt = 0;
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
    // seed_load[i] the number of tree branches seeds[i] already drives. A
    // negative seed_delay marks a tree wire the arc may not attach to or
    // pass through (it keeps its driver but is excluded from this search).
    // shell_tiles holds packed tiles (pack_shell_tile) indexed by each
    // task's shell_off/shell_cnt.
    virtual void route(const RouteParams &params, const std::vector<TaskDesc> &tasks,
                       const std::vector<ArcDesc> &arcs, const std::vector<int32_t> &seeds,
                       const std::vector<float> &seed_delay, const std::vector<float> &seed_load,
                       const std::vector<int32_t> &shell_tiles, bool large_lane, std::vector<ArcResult> &results,
                       std::vector<PathEntry> &paths) = 0;

    virtual const char *name() const = 0;
    // Device backends report the selected runtime index and the hardware name
    // after init(). CPU backends keep the default index of -1.
    virtual int device_index() const { return -1; }
    virtual const char *device_name() const { return name(); }
    virtual const BackendStats &stats() const = 0;
};

// The CPU lane is a performance shortcut for tiny batches. It must not be
// created when required-GPU mode is active, even when the user configured a
// positive threshold.
inline bool should_create_cpu_lane(int cpu_lane_nets, bool gpu_required, bool primary_is_cpu)
{
    return cpu_lane_nets > 0 && !gpu_required && !primary_is_cpu;
}

enum class BackendInitStatus
{
    ready,
    optional_failure,
    required_failure,
};

struct BackendInitResult
{
    BackendInitStatus status;
    bool cpu_fallback = false;
};

inline BackendInitStatus initialize_backend(Backend &backend, const GraphData &graph, int small_slots,
                                            int small_bits, int large_slots, int large_bits, bool gpu_required,
                                            std::string &error)
{
    if (backend.init(graph, small_slots, small_bits, large_slots, large_bits, error))
        return BackendInitStatus::ready;
    return gpu_required ? BackendInitStatus::required_failure : BackendInitStatus::optional_failure;
}

template <typename CpuBackendFactory>
inline BackendInitResult initialize_backend_with_cpu_fallback(
        std::unique_ptr<Backend> &backend, const GraphData &graph, int small_slots, int small_bits, int large_slots,
        int large_bits, bool gpu_required, bool gpu_backend_selected, CpuBackendFactory create_cpu_backend,
        std::string &error)
{
    auto status = initialize_backend(*backend, graph, small_slots, small_bits, large_slots, large_bits, gpu_required,
                                     error);
    if (status != BackendInitStatus::optional_failure || !gpu_backend_selected)
        return {status, false};

    const std::string gpu_error = error;
    std::unique_ptr<Backend> cpu_backend = create_cpu_backend();
    std::string cpu_error;
    if (!cpu_backend) {
        error = "GPU backend initialisation failed: " + gpu_error + "; CPU reference backend is unavailable";
        return {BackendInitStatus::optional_failure, false};
    }
    if (!cpu_backend->init(graph, small_slots, small_bits, large_slots, large_bits, cpu_error)) {
        error = "GPU backend initialisation failed: " + gpu_error + "; CPU reference backend initialisation failed: " +
                cpu_error;
        return {BackendInitStatus::optional_failure, false};
    }

    backend = std::move(cpu_backend);
    error = gpu_error;
    return {BackendInitStatus::ready, true};
}

// True if this build contains a GPU device backend.
bool device_backend_available();
// Human readable description of the GPU devices (empty if none).
std::string describe_devices();

// Create the device backend. device < 0 selects the device with the most
// compute units. Returns nullptr and sets error on failure.
std::unique_ptr<Backend> create_device_backend(int device, std::string &error);

// Sequential host reference implementation of the same algorithm.
std::unique_ptr<Backend> create_cpu_backend();

struct RoutingBackendSelection
{
    std::unique_ptr<Backend> backend;
    bool cpu_fallback = false;
};

// Select the GPU backend, honoring required mode. The CPU backend remains the
// default fallback unless the caller explicitly requires a device.
inline RoutingBackendSelection select_routing_backend(int device, bool cpu_requested, bool gpu_required,
                                                       std::string &error)
{
    if (cpu_requested) {
        if (gpu_required) {
            error = "the CPU reference backend was requested while GPU use is required";
            return {};
        }
        return {create_cpu_backend(), false};
    }

    auto backend = create_device_backend(device, error);
    if (backend)
        return {std::move(backend), false};
    if (gpu_required)
        return {};
    return {create_cpu_backend(), true};
}

} // namespace gpuroute

#endif // GPUROUTE_BACKEND_H
