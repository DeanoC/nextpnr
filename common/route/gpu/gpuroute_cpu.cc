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
 *  Sequential host reference implementation of the GPU routing backend.
 *
 *  It evaluates exactly the same cost function as the device kernel with a
 *  conventional binary-heap A*, so it serves three purposes: a fallback when
 *  no GPU is present, a correctness oracle for the device kernel, and a
 *  single-threaded speed baseline for the same algorithm.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <queue>
#include <unordered_map>

#include "gpuroute_backend.h"

namespace gpuroute {

namespace {

struct Visit
{
    float g;
    int32_t parent; // wire index, -1 for seeds
};

struct QEntry
{
    float f;
    float g;
    int32_t wire;
    int32_t parent;
    bool operator>(const QEntry &o) const
    {
        if (f != o.f)
            return f > o.f;
        if (g != o.g)
            return g > o.g;
        if (wire != o.wire)
            return wire > o.wire;
        return parent > o.parent;
    }
};

class CpuBackend : public Backend
{
  public:
    bool init(const GraphData &graph, int /*small_slots*/, int /*small_bits*/, int /*large_slots*/,
              int /*large_bits*/, std::string & /*error*/) override
    {
        g_ = graph;
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

    float edge_base_cost(int parent, int wire) const
    {
        for (int e = g_.out_off[parent]; e < g_.out_off[parent + 1]; e++)
            if (g_.out_dst[e] == wire)
                return g_.edge_cost[e];
        return 0.0f;
    }

    void route(const RouteParams &p, const std::vector<TaskDesc> &tasks, const std::vector<ArcDesc> &arcs,
               const std::vector<int32_t> &seeds, const std::vector<float> &seed_delay,
               const std::vector<float> &seed_load, bool /*large_lane*/, std::vector<ArcResult> &results,
               std::vector<PathEntry> &paths) override
    {
        auto t0 = std::chrono::steady_clock::now();
        results.assign(arcs.size(), ArcResult());
        size_t total_paths = 0;
        for (auto &t : tasks)
            total_paths = std::max(total_paths, (size_t)t.path_off + (size_t)t.path_cap);
        paths.assign(total_paths, PathEntry{-1, -1, 0.0f});

        std::unordered_map<int32_t, Visit> visited;
        std::unordered_map<int32_t, float> tree_delay, tree_load;
        std::priority_queue<QEntry, std::vector<QEntry>, std::greater<QEntry>> queue;
        std::vector<int32_t> tree;

        for (const auto &task : tasks) {
            tree.assign(seeds.begin() + task.tree_off, seeds.begin() + task.tree_off + task.tree_cnt);
            tree_delay.clear();
            tree_load.clear();
            for (int i = 0; i < task.tree_cnt; i++) {
                tree_delay[seeds[task.tree_off + i]] = seed_delay[task.tree_off + i];
                tree_load[seeds[task.tree_off + i]] = seed_load[task.tree_off + i];
            }
            int path_pos = 0;
            for (int a = 0; a < task.arc_cnt; a++) {
                const ArcDesc &arc = arcs[task.arc_off + a];
                ArcResult &r = results[task.arc_off + a];
                r.path_off = task.path_off + path_pos;
                const int sink = arc.sink;
                const int sx = g_.wire_x[sink], sy = g_.wire_y[sink];
                auto h = [&](int w) {
                    return p.est_weight *
                           (p.est_x * (float)std::abs(g_.wire_x[w] - sx) + p.est_y * (float)std::abs(g_.wire_y[w] - sy));
                };
                visited.clear();
                while (!queue.empty())
                    queue.pop();
                const float seed_scale =
                        p.seed_delay_weight * (p.seed_delay_floor + (1.0f - p.seed_delay_floor) * arc.crit);
                for (int w : tree) {
                    if (w == sink)
                        continue;
                    float g0 = seed_scale * tree_delay.at(w);
                    if (visited.emplace(w, Visit{g0, -1}).second)
                        queue.push(QEntry{g0 + h(w), g0, w, -1});
                }
                bool found = false;
                float best_g = 0.0f;
                int32_t best_parent = -1;
                int expanded = 0;
                while (!queue.empty()) {
                    QEntry cur = queue.top();
                    queue.pop();
                    if (found && cur.f >= best_g)
                        break;
                    auto vit = visited.find(cur.wire);
                    if (vit == visited.end() || vit->second.g != cur.g)
                        continue;
                    expanded++;
                    const int u = cur.wire;
                    float branch_extra = 0.0f;
                    if (p.load_penalty > 0.0f && vit->second.parent == -1) {
                        auto tl = tree_load.find(u);
                        if (tl != tree_load.end())
                            branch_extra = p.load_penalty * tl->second;
                    }
                    for (int e = g_.out_off[u]; e < g_.out_off[u + 1]; e++) {
                        const int v = g_.out_dst[e];
                        float c = g_.edge_cost[e];
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
                        const float hist = 1.0f + arc.crit_weight * (hist_[v] - 1.0f);
                        const float pres = 1.0f + (float)occ_[v] * p.curr_cong_weight * arc.crit_weight;
                        const float bias = p.bias_factor * (c / (float)task.fanout) *
                                           ((float)(std::abs(vx - task.cx) + std::abs(vy - task.cy)) / (float)task.hpwl);
                        const float gv = cur.g + c * hist * pres + bias + branch_extra;
                        if (v == sink) {
                            if (!found || gv < best_g || (gv == best_g && u < best_parent)) {
                                found = true;
                                best_g = gv;
                                best_parent = u;
                            }
                            continue;
                        }
                        const float f = gv + h(v);
                        if (found && f >= best_g)
                            continue;
                        auto ins = visited.emplace(v, Visit{gv, u});
                        if (!ins.second) {
                            Visit &old = ins.first->second;
                            if (old.parent == -1)
                                continue; // tree wire keeps its driver
                            if (gv < old.g || (gv == old.g && u < old.parent)) {
                                old.g = gv;
                                old.parent = u;
                            } else {
                                continue;
                            }
                        }
                        queue.push(QEntry{f, gv, v, u});
                    }
                }
                r.expanded = expanded;
                stats_.arcs_routed++;
                stats_.wires_expanded += expanded;
                if (!found) {
                    r.status = ARC_NO_PATH;
                    continue;
                }
                r.cost = best_g;
                r.status = ARC_OK;
                int32_t cur = sink, par = best_parent;
                int pos = path_pos;
                while (true) {
                    if (pos >= task.path_cap) {
                        r.status = ARC_PATH_FULL;
                        break;
                    }
                    paths[task.path_off + pos] = PathEntry{cur, par, 0.0f};
                    pos++;
                    const Visit &pv = visited.at(par);
                    if (pv.parent == -1)
                        break;
                    cur = par;
                    par = pv.parent;
                }
                if (r.status != ARC_OK)
                    continue;
                r.path_len = pos - path_pos;
                float d = tree_delay.at(paths[task.path_off + pos - 1].parent);
                for (int i = pos - 1; i >= path_pos; i--) {
                    PathEntry &pe = paths[task.path_off + i];
                    d += edge_base_cost(pe.parent, pe.wire);
                    pe.delay = d;
                    tree.push_back(pe.wire);
                    tree_delay[pe.wire] = d;
                }
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
    std::vector<int32_t> occ_, reserved_;
    std::vector<float> hist_;
    BackendStats stats_;
};

} // namespace

std::unique_ptr<Backend> create_cpu_backend() { return std::unique_ptr<Backend>(new CpuBackend()); }

} // namespace gpuroute
