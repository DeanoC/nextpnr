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
 *  Host implementation of the GPU routing backend.
 *
 *  This is a scalar transcription of the device kernel in
 *  gpuroute_kernel.cuh: the same K-best stepping (frontier compaction,
 *  histogram threshold, two-phase relaxation, minimum over (g, edge)), the
 *  same cost expressions in the same evaluation order, and the same
 *  tie-breaking. A machine without a GPU therefore produces the same routes
 *  as one with a GPU, and the two backends can check each other. It also
 *  serves as the fallback backend and as a speed baseline.
 *
 *  The tasks of a batch are independent (they read one congestion snapshot
 *  and write disjoint result and path regions, exactly as the blocks of a
 *  device launch do), so they are routed on several threads; the arcs of a
 *  task stay sequential. Each thread owns an open-addressing table and its
 *  piles, so the result does not depend on the thread count or scheduling.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

#include "gpuroute_backend.h"

namespace gpuroute {

namespace {

constexpr int NBINS = 128;                  // as in the kernel
constexpr uint32_t NONE_EDGE = 0xFFFFFFFFu; // "parent" marker of a seed wire
constexpr float INF = std::numeric_limits<float>::infinity();

struct Entry
{
    float g = INF;
    uint32_t lo = NONE_EDGE; // parent edge, or NONE_EDGE for a tree wire
    float sdelay = 0.0f;     // upstream base delay of a seed
    float sload = 0.0f;      // existing branch count of a seed
};

inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// wire -> Entry, open addressing with linear probing. The table grows as a
// search needs it and is reset by clearing only the slots it used, so a
// small search after a large one does not pay for the large table. Only
// the entry count is observable to the search (the device's load limit is
// emulated from it); the slot layout is not.
class Table
{
  public:
    Table() { reserve(1 << 12); }

    void reset()
    {
        for (uint32_t s : used_)
            keys_[s] = -1;
        used_.clear();
    }
    size_t size() const { return used_.size(); }

    int find(int32_t w) const
    {
        uint32_t i = hash32(uint32_t(w)) & mask_;
        while (true) {
            const int32_t k = keys_[i];
            if (k == w)
                return int(i);
            if (k == -1)
                return -1;
            i = (i + 1) & mask_;
        }
    }

    // Slot of w, created with a default Entry when absent
    int insert(int32_t w, bool &inserted)
    {
        if ((used_.size() + 1) * 2 > keys_.size())
            grow();
        uint32_t i = hash32(uint32_t(w)) & mask_;
        while (true) {
            const int32_t k = keys_[i];
            if (k == w) {
                inserted = false;
                return int(i);
            }
            if (k == -1) {
                keys_[i] = w;
                vals_[i] = Entry();
                used_.push_back(i);
                inserted = true;
                return int(i);
            }
            i = (i + 1) & mask_;
        }
    }

    Entry &at(int slot) { return vals_[slot]; }
    const Entry &at(int slot) const { return vals_[slot]; }

  private:
    void reserve(size_t cap)
    {
        keys_.assign(cap, -1);
        vals_.resize(cap);
        mask_ = uint32_t(cap - 1);
    }
    void grow()
    {
        std::vector<int32_t> old_keys = std::move(keys_);
        std::vector<Entry> old_vals = std::move(vals_);
        std::vector<uint32_t> old_used = std::move(used_);
        reserve(old_keys.size() * 2);
        used_.reserve(old_used.size() * 2);
        for (uint32_t s : old_used) {
            uint32_t i = hash32(uint32_t(old_keys[s])) & mask_;
            while (keys_[i] != -1)
                i = (i + 1) & mask_;
            keys_[i] = old_keys[s];
            vals_[i] = old_vals[s];
            used_.push_back(i);
        }
    }

    std::vector<int32_t> keys_; // -1: empty
    std::vector<Entry> vals_;
    std::vector<uint32_t> used_;
    uint32_t mask_ = 0;
};

struct Pile
{
    float g;
    float f; // g + weighted estimate, valid after frontier compaction
    int32_t wire;
};

// Only the opt-in exact search uses this heap. The approximate backend
// retains the GPU kernel's frontier compaction and K-best stepping.
struct ExactPile
{
    float f, g;
    int32_t wire;
};
struct ExactLater
{
    bool operator()(const ExactPile &a, const ExactPile &b) const
    {
        return a.f > b.f || (a.f == b.f && a.wire > b.wire);
    }
};

// Lexicographic (g, lo) order, matching the kernel's packed 64-bit compare
inline bool less_pair(float g1, uint32_t lo1, float g2, uint32_t lo2)
{
    if (g1 != g2)
        return g1 < g2;
    return lo1 < lo2;
}

struct Cand
{
    float g;
    int32_t wire;
    uint32_t edge;
};

// Everything the relaxation of an edge reads about its destination wire, in
// one 16-byte record so a random wire costs one cache line, not five
struct WireRec
{
    int16_t x, y;
    uint8_t flags;
    uint8_t pad[3];
    int32_t reserved;
    int32_t occ;
    float hist;
};

// Per-thread search state
struct Scratch
{
    Table table;
    std::vector<Pile> far, far2, near;
    std::vector<ExactPile> exact_frontier;
    std::vector<Cand> cand;
    std::vector<int> hist = std::vector<int>(NBINS);
};

class CpuBackend : public Backend
{
  public:
    CpuBackend()
    {
        unsigned n = std::thread::hardware_concurrency();
        threads_ = n == 0 ? 4 : int(std::min<unsigned>(n, 32));
    }

    bool init(const GraphData &graph, int /*small_slots*/, int small_bits, int /*large_slots*/, int large_bits,
              std::string & /*error*/) override
    {
        g_ = graph;
        // A null region is "no architecture-reserved pips". Materialise zeros
        // once so the search can load edge_region[ei] on every usable edge.
        if (g_.edge_region == nullptr && g_.n_edges > 0) {
            edge_region_none_.assign((size_t)g_.n_edges, 0);
            g_.edge_region = edge_region_none_.data();
        } else {
            edge_region_none_.clear();
        }
        // Same stand-in for the halo fence: a null list is no outside-halo pips.
        if (g_.edge_outside_halo == nullptr && g_.n_edges > 0) {
            edge_outside_halo_none_.assign((size_t)g_.n_edges, 0);
            g_.edge_outside_halo = edge_outside_halo_none_.data();
        } else {
            edge_outside_halo_none_.clear();
        }
        // The path walk needs the source wire of an edge; one load beats a
        // binary search over the row offsets for every hop.
        edge_src_.resize((size_t)g_.n_edges);
        for (int32_t w = 0; w < g_.n_wires; w++)
            for (int32_t e = g_.out_off[w]; e < g_.out_off[w + 1]; e++)
                edge_src_[e] = w;
        small_cap_ = 1 << small_bits;
        large_cap_ = 1 << large_bits;
        wires_.resize((size_t)graph.n_wires);
        for (int32_t w = 0; w < graph.n_wires; w++) {
            WireRec &r = wires_[w];
            r.x = graph.wire_x[w];
            r.y = graph.wire_y[w];
            r.flags = graph.wire_flags[w];
            r.pad[0] = r.pad[1] = r.pad[2] = 0;
            r.reserved = graph.wire_reserved[w];
            r.occ = graph.wire_occ[w];
            r.hist = graph.wire_hist[w];
        }
        return true;
    }

    void set_threads(int n) override { threads_ = std::max(1, n); }

    void update_wire_state(size_t count, const int32_t *wires, const int32_t *occ, const float *hist,
                           const int32_t *reserved) override
    {
        for (size_t i = 0; i < count; i++) {
            WireRec &r = wires_[wires[i]];
            r.occ = occ[i];
            r.hist = hist[i];
            r.reserved = reserved[i];
        }
    }

    // Source wire of CSR edge e
    int edge_src(int e) const { return edge_src_[e]; }

    void route(const RouteParams &p, const std::vector<TaskDesc> &tasks, const std::vector<ArcDesc> &arcs,
               const std::vector<int32_t> &seeds, const std::vector<float> &seed_delay,
               const std::vector<float> &seed_load, const std::vector<int32_t> &shell_tiles, bool large_lane,
               std::vector<ArcResult> &results, std::vector<PathEntry> &paths) override
    {
        auto t0 = std::chrono::steady_clock::now();
        // The device fails an arc once its hash table passes 60 % load and
        // the host re-routes it in the large lane after the rest of the
        // batch was applied. Emulate that limit so both backends take the
        // same route through the retry lanes.
        const int cap = large_lane ? large_cap_ : small_cap_;
        const size_t load_limit = size_t((cap / 5) * 3) + 1;
        results.assign(arcs.size(), ArcResult());
        size_t total_paths = 0;
        for (auto &t : tasks)
            total_paths = std::max(total_paths, (size_t)t.path_off + (size_t)t.path_cap);
        paths.assign(total_paths, PathEntry{-1, -1, -1, 0.0f});

        std::atomic<size_t> next_task{0};
        auto worker = [&](Scratch &sc) {
            while (true) {
                const size_t ti = next_task.fetch_add(1);
                if (ti >= tasks.size())
                    break;
                route_task(sc, p, tasks[ti], arcs, seeds, seed_delay, seed_load, shell_tiles.data(), load_limit,
                           results, paths);
            }
        };
        int nt = int(std::min<size_t>(size_t(threads_), tasks.size()));
#ifdef NPNR_DISABLE_THREADS
        nt = 1; // e.g. the WASI build: pthread_create() always fails there
#endif
        if (scratch_.size() < size_t(std::max(nt, 1)))
            scratch_.resize(std::max(nt, 1));
        if (nt <= 1) {
            worker(scratch_[0]);
        } else {
#ifndef NPNR_DISABLE_THREADS
            std::vector<std::thread> pool;
            pool.reserve(nt - 1);
            for (int t = 1; t < nt; t++)
                pool.emplace_back([&, t]() { worker(scratch_[t]); });
            worker(scratch_[0]);
            for (auto &th : pool)
                th.join();
#endif
        }

        stats_.launches++;
        stats_.arcs_routed += int64_t(arcs.size());
        for (const auto &r : results)
            stats_.wires_expanded += r.expanded;
        stats_.route_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    const char *name() const override { return "cpu-reference"; }
    const BackendStats &stats() const override { return stats_; }

  private:
    // One task: its arcs in order, each seeded from the tree and the paths
    // of the arcs before it, exactly as one device block does it.
    void route_task(Scratch &sc, const RouteParams &p, const TaskDesc &task, const std::vector<ArcDesc> &arcs,
                    const std::vector<int32_t> &seeds, const std::vector<float> &seed_delay,
                    const std::vector<float> &seed_load, const int32_t *shells, size_t load_limit,
                    std::vector<ArcResult> &results, std::vector<PathEntry> &paths) const
    {
        Table &table = sc.table;
        auto &far = sc.far;
        auto &far2 = sc.far2;
        auto &near = sc.near;
        auto &exact_frontier = sc.exact_frontier;
        auto &cand = sc.cand;
        auto &hist = sc.hist;
        int path_pos = 0;
        for (int a = 0; a < task.arc_cnt; a++) {
            const ArcDesc &arc = arcs[task.arc_off + a];
            ArcResult &r = results[task.arc_off + a];
            r.path_off = task.path_off + path_pos;
            const int sink = arc.sink;
            const WireRec *wires = wires_.data();
            const int sx = wires[sink].x, sy = wires[sink].y;
            auto h_est = [&](int x, int y) {
                int dx = x - sx, dy = y - sy;
                dx = dx < 0 ? -dx : dx;
                dy = dy < 0 ? -dy : dy;
                return p.est_weight * (p.est_x * (float)dx + p.est_y * (float)dy);
            };

            // 1./2. table and seeds
            table.reset();
            far.clear();
            exact_frontier.clear();
            auto exact_push = [&](float g, int32_t wire) {
                const float f = g + h_est(wires[wire].x, wires[wire].y);
                // The scan reference never chooses a NaN as its minimum.
                if (std::isnan(f))
                    return;
                exact_frontier.push_back(ExactPile{f, g, wire});
                std::push_heap(exact_frontier.begin(), exact_frontier.end(), ExactLater());
            };
            const float seed_scale =
                    p.seed_delay_weight * (p.seed_delay_floor + (1.0f - p.seed_delay_floor) * arc.crit);
            const int nseed = task.tree_cnt + path_pos;
            bool reached = false;
            for (int i = 0; i < nseed; i++) {
                int w;
                float d, ld = 0.0f;
                if (i < task.tree_cnt) {
                    w = seeds[task.tree_off + i];
                    d = seed_delay[task.tree_off + i];
                    ld = seed_load[task.tree_off + i];
                } else {
                    const PathEntry &pe = paths[task.path_off + (i - task.tree_cnt)];
                    w = pe.wire;
                    d = pe.delay;
                }
                // Not an attach point: this wire is the arc's destination.
                // On the path of an earlier arc of the task it is already
                // routed, and the arc shares that branch (see below).
                if (w == sink) {
                    reached |= i >= task.tree_cnt;
                    continue;
                }
                bool inserted;
                const int s = table.insert(w, inserted);
                if (!inserted)
                    continue;
                Entry &e = table.at(s);
                if (d < 0.0f) {
                    e.g = INF; // blocked tree wire
                    continue;
                }
                e.g = seed_scale * d;
                e.sdelay = d;
                e.sload = ld;
                if (p.exact)
                    exact_push(e.g, w);
                else
                    far.push_back(Pile{e.g, 0.0f, w});
            }
            // An earlier arc already reaches the sink: routed, with no
            // new wires. A searched path would give the sink a second
            // parent, and later arcs would seed from wires the host
            // never binds.
            if (reached) {
                r.status = ARC_OK;
                continue;
            }

            bool have_best = false;
            float best_g = INF;
            uint32_t best_edge = NONE_EDGE;
            float prune = INF;
            int expanded = 0, steps = 0;
            bool overflow = false;

            // 3. K-best steps
            while (!overflow) {
                if (p.exact) {
                    // Same lowest-(f,wire) choice as the scan reference,
                    // without rescanning the entire frontier per expansion.
                    // An improved g leaves an obsolete heap entry; discard
                    // it using the same equality check as scan compaction.
                    near.clear();
                    while (!exact_frontier.empty()) {
                        const auto e = exact_frontier.front();
                        const int s = table.find(e.wire);
                        if (s >= 0 && table.at(s).g == e.g && e.f < prune) {
                            if (have_best && e.f >= best_g)
                                break;
                            near.push_back(Pile{e.g, e.f, e.wire});
                        }
                        std::pop_heap(exact_frontier.begin(), exact_frontier.end(), ExactLater());
                        exact_frontier.pop_back();
                        if (!near.empty())
                            break;
                    }
                    if (near.empty())
                        break;
                } else {
                    // pass 1: compact the frontier, track the f range
                    far2.clear();
                    float minf = INF, maxf = 0.0f;
                    for (const Pile &e : far) {
                        const int s = table.find(e.wire);
                        if (s < 0 || table.at(s).g != e.g)
                            continue;
                        const float f = e.g + h_est(wires[e.wire].x, wires[e.wire].y);
                        if (f >= prune)
                            continue;
                        minf = std::min(minf, f);
                        maxf = std::max(maxf, f);
                        far2.push_back(Pile{e.g, f, e.wire});
                    }
                    if (far2.empty() || minf == INF || (have_best && minf >= best_g))
                        break;
                    const float range = maxf - minf;
                    const float binw = range > 1e-6f ? range / (float)NBINS : 0.0f;

                    // pass 2: histogram and threshold
                    float thr = INF;
                    if (binw > 0.0f) {
                        std::fill(hist.begin(), hist.end(), 0);
                        for (const Pile &e : far2) {
                            int b = (int)((e.f - minf) / binw);
                            b = b < 0 ? 0 : (b >= NBINS ? NBINS - 1 : b);
                            hist[b]++;
                        }
                        int cum = 0;
                        int want = p.expand_k;
                        if (p.expand_div > 0 && int(far2.size()) / p.expand_div > want)
                            want = int(far2.size()) / p.expand_div;
                        for (int b = 0; b < NBINS; b++) {
                            cum += hist[b];
                            if (cum >= want) {
                                thr = minf + (float)(b + 1) * binw;
                                break;
                            }
                        }
                    }

                    // pass 3: split into this step's expansion set and the rest
                    near.clear();
                    far.clear();
                    for (const Pile &e : far2) {
                        // The bin boundary can round back to minf when the range
                        // is small relative to the costs. Always expand the minima
                        // so a nonempty frontier cannot repeat without progress.
                        if (e.f < thr || e.f == minf)
                            near.push_back(e);
                        else
                            far.push_back(e);
                    }
                }
                prune = have_best ? best_g : INF;

                // phase A: relax against the step-start table
                cand.clear();
                for (const Pile &e : near) {
                    const int32_t u = e.wire;
                    const float gu = e.g;
                    expanded++;
                    float branch_extra = 0.0f;
                    if (p.load_penalty > 0.0f) {
                        const int su = table.find(u);
                        if (su >= 0 && table.at(su).lo == NONE_EDGE)
                            branch_extra = p.load_penalty * table.at(su).sload;
                    }
                    for (int ei = g_.out_off[u]; ei < g_.out_off[u + 1]; ei++) {
                        const int v = g_.out_dst[ei];
                        float c = g_.edge_cost[ei];
                        if (c < 0.0f || (g_.edge_region[ei] && !task.allow_reserved))
                            continue;
                        c += p.pip_adder;
                        const WireRec &wv = wires[v];
                        const int vx = wv.x, vy = wv.y;
                        if (task.stay_in_halo && g_.edge_outside_halo[ei] &&
                            !shell_tile_hit(shells, task.shell_off, task.shell_cnt, vx, vy))
                            continue;
                        if (p.use_bb && (vx < task.bb_x0 || vx > task.bb_x1 || vy < task.bb_y0 || vy > task.bb_y1))
                            continue;
                        if (wv.flags & WIRE_UNAVAILABLE)
                            continue;
                        const int rsv = wv.reserved;
                        if (rsv != -1 && (rsv & ~RESERVED_SOFT) != task.net &&
                            !(p.ignore_soft && (rsv & RESERVED_SOFT)))
                            continue;
                        const float hist_c = p.ignore_hist ? 1.0f : 1.0f + arc.crit_weight * (wv.hist - 1.0f);
                        const float pres = 1.0f + (float)wv.occ * p.curr_cong_weight * arc.crit_weight;
                        int bdx = vx - task.cx, bdy = vy - task.cy;
                        bdx = bdx < 0 ? -bdx : bdx;
                        bdy = bdy < 0 ? -bdy : bdy;
                        const float bias =
                                p.bias_factor * (c / (float)task.fanout) * ((float)(bdx + bdy) / (float)task.hpwl);
                        const float gv = gu + c * hist_c * pres + bias + branch_extra;
                        if (v == sink) {
                            if (!have_best || less_pair(gv, (uint32_t)ei, best_g, best_edge)) {
                                have_best = true;
                                best_g = gv;
                                best_edge = (uint32_t)ei;
                            }
                            continue;
                        }
                        const float f = gv + h_est(vx, vy);
                        if (f >= prune)
                            continue;
                        const int sv = table.find(v);
                        if (sv >= 0) {
                            const Entry &cur = table.at(sv);
                            if (cur.lo == NONE_EDGE || !less_pair(gv, (uint32_t)ei, cur.g, cur.lo))
                                continue;
                        }
                        cand.push_back(Cand{gv, v, (uint32_t)ei});
                    }
                }

                // phase B: apply the candidates as a minimum over (g, edge)
                for (const Cand &cd : cand) {
                    bool inserted;
                    const int s = table.insert(cd.wire, inserted);
                    if (inserted && table.size() > load_limit)
                        overflow = true;
                    Entry &e = table.at(s);
                    const float old_g = e.g;
                    if (less_pair(cd.g, cd.edge, e.g, e.lo)) {
                        e.g = cd.g;
                        e.lo = cd.edge;
                    }
                    // re-queue only when g itself improves
                    if (cd.g < old_g) {
                        if (p.exact)
                            exact_push(cd.g, cd.wire);
                        else
                            far.push_back(Pile{cd.g, 0.0f, cd.wire});
                    }
                }
                steps++;
            }

            r.expanded = expanded;
            r.steps = steps;
            if (overflow) {
                r.status = ARC_OVERFLOW;
                r.reason = FAIL_LOAD;
                continue;
            }
            if (!have_best) {
                r.status = ARC_NO_PATH;
                continue;
            }

            // 4. path
            r.cost = best_g;
            r.status = ARC_OK;
            uint32_t edge = best_edge;
            int32_t cur = sink;
            int pos = path_pos;
            float attach_delay = 0.0f;
            while (true) {
                if (pos >= task.path_cap) {
                    r.status = ARC_PATH_FULL;
                    break;
                }
                const int32_t par = edge_src((int)edge);
                paths[task.path_off + pos] = PathEntry{cur, par, (int32_t)edge, 0.0f};
                pos++;
                const Entry &pe = table.at(table.find(par));
                if (pe.lo == NONE_EDGE) {
                    attach_delay = pe.sdelay;
                    break;
                }
                cur = par;
                edge = pe.lo;
            }
            if (r.status != ARC_OK)
                continue;
            float d = attach_delay;
            for (int k = pos - 1; k >= path_pos; k--) {
                PathEntry &pe = paths[task.path_off + k];
                d += g_.edge_cost[pe.edge];
                pe.delay = d;
            }
            r.path_len = pos - path_pos;
            path_pos = pos;
        }
    }

    GraphData g_;
    int small_cap_ = 1 << 16, large_cap_ = 1 << 22;
    int threads_ = 1;
    std::vector<uint8_t> edge_region_none_;       // owned stand-in when GraphData::edge_region is null
    std::vector<uint8_t> edge_outside_halo_none_; // stand-in when edge_outside_halo is null
    std::vector<int32_t> edge_src_; // n_edges, source wire of each CSR edge
    std::vector<WireRec> wires_;     // n_wires
    std::vector<Scratch> scratch_;
    BackendStats stats_;
};

} // namespace

std::unique_ptr<Backend> create_cpu_backend() { return std::unique_ptr<Backend>(new CpuBackend()); }

} // namespace gpuroute
