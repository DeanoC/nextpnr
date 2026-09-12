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
 *  Sequential host implementation of the GPU routing backend.
 *
 *  This is a scalar transcription of the device kernel in
 *  gpuroute_kernel.cuh: the same K-best stepping (frontier compaction,
 *  histogram threshold, two-phase relaxation, minimum over (g, edge)), the
 *  same cost expressions in the same evaluation order, and the same
 *  tie-breaking. A machine without a GPU therefore produces the same routes
 *  as one with a GPU, and the two backends can check each other. It also
 *  serves as the fallback backend and as a single-threaded speed baseline.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

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

struct Pile
{
    float g;
    int32_t wire;
};

// Lexicographic (g, lo) order, matching the kernel's packed 64-bit compare
inline bool less_pair(float g1, uint32_t lo1, float g2, uint32_t lo2)
{
    if (g1 != g2)
        return g1 < g2;
    return lo1 < lo2;
}

class CpuBackend : public Backend
{
  public:
    bool init(const GraphData &graph, int /*small_slots*/, int small_bits, int /*large_slots*/, int large_bits,
              std::string & /*error*/) override
    {
        g_ = graph;
        small_cap_ = 1 << small_bits;
        large_cap_ = 1 << large_bits;
        occ_.assign(graph.wire_occ, graph.wire_occ + graph.n_wires);
        hist_.assign(graph.wire_hist, graph.wire_hist + graph.n_wires);
        reserved_.assign(graph.wire_reserved, graph.wire_reserved + graph.n_wires);
        return true;
    }

    void update_wire_state(size_t count, const int32_t *wires, const int32_t *occ, const float *hist,
                           const int32_t *reserved) override
    {
        for (size_t i = 0; i < count; i++) {
            occ_[wires[i]] = occ[i];
            hist_[wires[i]] = hist[i];
            reserved_[wires[i]] = reserved[i];
        }
    }

    int edge_src(int e) const
    {
        // largest wire whose CSR row starts at or before e
        auto it = std::upper_bound(g_.out_off, g_.out_off + g_.n_wires + 1, e);
        return int(it - g_.out_off) - 1;
    }

    void route(const RouteParams &p, const std::vector<TaskDesc> &tasks, const std::vector<ArcDesc> &arcs,
               const std::vector<int32_t> &seeds, const std::vector<float> &seed_delay,
               const std::vector<float> &seed_load, bool large_lane, std::vector<ArcResult> &results,
               std::vector<PathEntry> &paths) override
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

        std::unordered_map<int32_t, Entry> table;
        std::vector<Pile> far, far2, near;
        struct Cand
        {
            float g;
            int32_t wire;
            uint32_t edge;
        };
        std::vector<Cand> cand;
        std::vector<int> hist(NBINS);

        for (const auto &task : tasks) {
            int path_pos = 0;
            for (int a = 0; a < task.arc_cnt; a++) {
                const ArcDesc &arc = arcs[task.arc_off + a];
                ArcResult &r = results[task.arc_off + a];
                r.path_off = task.path_off + path_pos;
                const int sink = arc.sink;
                const int sx = g_.wire_x[sink], sy = g_.wire_y[sink];
                auto h_est = [&](int x, int y) {
                    int dx = x - sx, dy = y - sy;
                    dx = dx < 0 ? -dx : dx;
                    dy = dy < 0 ? -dy : dy;
                    return p.est_weight * (p.est_x * (float)dx + p.est_y * (float)dy);
                };

                // 1./2. table and seeds
                table.clear();
                far.clear();
                const float seed_scale =
                        p.seed_delay_weight * (p.seed_delay_floor + (1.0f - p.seed_delay_floor) * arc.crit);
                const int nseed = task.tree_cnt + path_pos;
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
                    if (w == sink)
                        continue;
                    auto ins = table.emplace(w, Entry());
                    if (!ins.second)
                        continue;
                    Entry &e = ins.first->second;
                    if (d < 0.0f) {
                        e.g = INF; // blocked tree wire
                        continue;
                    }
                    e.g = seed_scale * d;
                    e.sdelay = d;
                    e.sload = ld;
                    far.push_back(Pile{e.g, w});
                }

                bool have_best = false;
                float best_g = INF;
                uint32_t best_edge = NONE_EDGE;
                float prune = INF;
                int expanded = 0, steps = 0;
                bool overflow = false;

                // 3. K-best steps
                while (!overflow) {
                    // pass 1: compact the frontier, track the f range
                    far2.clear();
                    float minf = INF, maxf = 0.0f;
                    for (const Pile &e : far) {
                        auto it = table.find(e.wire);
                        if (it == table.end() || it->second.g != e.g)
                            continue;
                        const float f = e.g + h_est(g_.wire_x[e.wire], g_.wire_y[e.wire]);
                        if (f >= prune)
                            continue;
                        minf = std::min(minf, f);
                        maxf = std::max(maxf, f);
                        far2.push_back(e);
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
                            const float f = e.g + h_est(g_.wire_x[e.wire], g_.wire_y[e.wire]);
                            int b = (int)((f - minf) / binw);
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
                        const float f = e.g + h_est(g_.wire_x[e.wire], g_.wire_y[e.wire]);
                        if (f < thr)
                            near.push_back(e);
                        else
                            far.push_back(e);
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
                            auto su = table.find(u);
                            if (su != table.end() && su->second.lo == NONE_EDGE)
                                branch_extra = p.load_penalty * su->second.sload;
                        }
                        for (int ei = g_.out_off[u]; ei < g_.out_off[u + 1]; ei++) {
                            const int v = g_.out_dst[ei];
                            float c = g_.edge_cost[ei];
                            if (c < 0.0f)
                                continue;
                            c += p.pip_adder;
                            const int vx = g_.wire_x[v], vy = g_.wire_y[v];
                            if (p.use_bb && (vx < task.bb_x0 || vx > task.bb_x1 || vy < task.bb_y0 || vy > task.bb_y1))
                                continue;
                            if (g_.wire_flags[v] & WIRE_UNAVAILABLE)
                                continue;
                            const int rsv = reserved_[v];
                            if (rsv != -1 && (rsv & ~RESERVED_SOFT) != task.net &&
                                !(p.ignore_soft && (rsv & RESERVED_SOFT)))
                                continue;
                            const float hist_c = 1.0f + arc.crit_weight * (hist_[v] - 1.0f);
                            const float pres = 1.0f + (float)occ_[v] * p.curr_cong_weight * arc.crit_weight;
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
                            auto sv = table.find(v);
                            if (sv != table.end()) {
                                const Entry &cur = sv->second;
                                if (cur.lo == NONE_EDGE || !less_pair(gv, (uint32_t)ei, cur.g, cur.lo))
                                    continue;
                            }
                            cand.push_back(Cand{gv, v, (uint32_t)ei});
                        }
                    }

                    // phase B: apply the candidates as a minimum over (g, edge)
                    for (const Cand &cd : cand) {
                        auto ins = table.emplace(cd.wire, Entry());
                        if (ins.second && table.size() > load_limit)
                            overflow = true;
                        Entry &e = ins.first->second;
                        const float old_g = e.g;
                        if (less_pair(cd.g, cd.edge, e.g, e.lo)) {
                            e.g = cd.g;
                            e.lo = cd.edge;
                        }
                        // re-queue only when g itself improves
                        if (cd.g < old_g)
                            far.push_back(Pile{cd.g, cd.wire});
                    }
                    steps++;
                }

                r.expanded = expanded;
                r.steps = steps;
                stats_.arcs_routed++;
                stats_.wires_expanded += expanded;
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
                    const Entry &pe = table.at(par);
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
        stats_.launches++;
        stats_.route_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    const char *name() const override { return "cpu-reference"; }
    const BackendStats &stats() const override { return stats_; }

  private:
    GraphData g_;
    int small_cap_ = 1 << 16, large_cap_ = 1 << 22;
    std::vector<int32_t> occ_, reserved_;
    std::vector<float> hist_;
    BackendStats stats_;
};

} // namespace

std::unique_ptr<Backend> create_cpu_backend() { return std::unique_ptr<Backend>(new CpuBackend()); }

} // namespace gpuroute
