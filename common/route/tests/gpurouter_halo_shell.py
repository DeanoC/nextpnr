#!/usr/bin/env python3
"""A halo-constrained task must not be offered pips outside its shell tiles.

The cheap path leaves the halo on the way to the sink. Binding would reject
that pip (fes_slot_route_outside_plug_halo_and_shell). The search has to take
the longer hop that stays inside the halo until it steps onto the sink tile,
and a null outside-halo list must keep the old behaviour.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CPU_BACKEND = ROOT / "common/route/gpu/gpuroute_cpu.cc"

DRIVER = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gpuroute_backend.h"

namespace {

// Two routes from wire 0 to wire 3:
//   0 -> 1 -> 3   stays in the halo, then steps onto the shell tile. cost 3+3
//   0 -> 2 -> 3   leaves the halo at wire 2 (tile 4,0). cost 1+1
// Wire 3 is the sink, on shell tile (6, 0).
const int32_t out_off[] = {0, 2, 3, 4, 4};
const int32_t out_dst[] = {1, 2, 3, 3};
const float edge_cost[] = {3.0f, 1.0f, 3.0f, 1.0f};
const uint8_t edge_outside[] = {0, 1, 1, 1};
const int16_t wire_x[] = {0, 1, 4, 6};
const int16_t wire_y[] = {0, 0, 0, 0};
const int32_t wire_reserved[] = {-1, -1, -1, -1};
const uint8_t wire_flags[] = {0, 0, 0, 0};
const int32_t wire_occ[] = {0, 0, 0, 0};
const float wire_hist[] = {1.0f, 1.0f, 1.0f, 1.0f};

struct Found {
    int status = -1;
    bool saw(int32_t wire) const {
        for (int32_t w : wires)
            if (w == wire)
                return true;
        return false;
    }
    std::vector<int32_t> wires;
};

Found route(int stay, const std::vector<std::pair<int, int>> &shell, const uint8_t *outside)
{
    std::string error;
    gpuroute::GraphData g;
    g.n_wires = 4;
    g.n_edges = 4;
    g.out_off = out_off;
    g.out_dst = out_dst;
    g.edge_cost = edge_cost;
    g.edge_outside_halo = outside;
    g.wire_x = wire_x;
    g.wire_y = wire_y;
    g.wire_reserved = wire_reserved;
    g.wire_flags = wire_flags;
    g.wire_occ = wire_occ;
    g.wire_hist = wire_hist;

    auto backend = gpuroute::create_cpu_backend();
    assert(backend && "cpu-reference backend must exist");
    if (!backend->init(g, 4, 12, 4, 20, error)) {
        std::fprintf(stderr, "backend init failed: %s\n", error.c_str());
        std::exit(2);
    }

    gpuroute::RouteParams params;
    params.use_bb = 0;
    params.exact = 1;
    params.expand_k = 1;
    params.bias_factor = 0.0f;

    std::vector<int32_t> packed;
    for (auto tile : shell)
        packed.push_back(gpuroute::pack_shell_tile(tile.first, tile.second));

    gpuroute::TaskDesc task;
    task.net = 0;
    task.tree_off = 0;
    task.tree_cnt = 1;
    task.arc_off = 0;
    task.arc_cnt = 1;
    task.path_off = 0;
    task.path_cap = 64;
    task.cx = 1;
    task.cy = 0;
    task.hpwl = 6;
    task.fanout = 1;
    task.stay_in_halo = stay;
    task.shell_off = 0;
    task.shell_cnt = int32_t(packed.size());

    std::vector<gpuroute::TaskDesc> tasks{task};
    std::vector<gpuroute::ArcDesc> arcs{{3, 1.0f, 0.0f}};
    std::vector<int32_t> seeds{0};
    std::vector<float> seed_delay{0.0f};
    std::vector<float> seed_load{0.0f};
    std::vector<gpuroute::ArcResult> results;
    std::vector<gpuroute::PathEntry> paths;

    backend->route(params, tasks, arcs, seeds, seed_delay, seed_load, packed, false, results, paths);
    assert(results.size() == 1);
    Found found;
    found.status = results[0].status;
    if (found.status == gpuroute::ARC_OK) {
        for (int32_t i = 0; i < results[0].path_len; i++) {
            const auto &e = paths.at(results[0].path_off + i);
            found.wires.push_back(e.wire);
            found.wires.push_back(e.parent);
        }
    }
    return found;
}

} // namespace

int main()
{
    const std::vector<std::pair<int, int>> sink_tile{{6, 0}};
    // Constrained: the cheap hop onto tile (4, 0) is outside the halo and is
    // not this net's shell tile, so the search takes the halo hop.
    Found legal = route(1, sink_tile, edge_outside);
    if (legal.status != gpuroute::ARC_OK || legal.saw(2) || !legal.saw(1)) {
        std::fprintf(stderr, "halo fence offered the outside pip or missed the legal hop\n");
        return 1;
    }
    // The same task with the fence off still prefers the cheaper outside pip.
    Found open = route(0, sink_tile, edge_outside);
    if (open.status != gpuroute::ARC_OK || !open.saw(2)) {
        std::fprintf(stderr, "stay_in_halo=0 did not take the cheaper outside pip\n");
        return 1;
    }
    // Landing on this net's own shell tile is the exception, so tile (4, 0)
    // becomes legal and the cheap route returns.
    Found extra = route(1, {{4, 0}, {6, 0}}, edge_outside);
    if (extra.status != gpuroute::ARC_OK || !extra.saw(2)) {
        std::fprintf(stderr, "shell-tile exception did not allow the outside pip\n");
        return 1;
    }
    // No outside-halo bits: the fence must not invent any.
    Found none = route(1, sink_tile, nullptr);
    if (none.status != gpuroute::ARC_OK || !none.saw(2)) {
        std::fprintf(stderr, "null edge_outside_halo was treated as outside the halo\n");
        return 1;
    }
    // The only route is 0 -> 2 -> 3. Edge 0 -> 2 lands on tile (4, 0), which
    // is outside the halo and is not the shell tile, so a constrained task
    // has no path. The same graph still routes when the fence is off.
    const int32_t only_off[] = {0, 1, 1, 2, 2};
    const int32_t only_dst[] = {2, 3};
    const float only_cost[] = {1.0f, 1.0f};
    const uint8_t only_outside[] = {1, 1};
    auto only = [&](int stay) {
        std::string error;
        gpuroute::GraphData g;
        g.n_wires = 4;
        g.n_edges = 2;
        g.out_off = only_off;
        g.out_dst = only_dst;
        g.edge_cost = only_cost;
        g.edge_outside_halo = only_outside;
        g.wire_x = wire_x;
        g.wire_y = wire_y;
        g.wire_reserved = wire_reserved;
        g.wire_flags = wire_flags;
        g.wire_occ = wire_occ;
        g.wire_hist = wire_hist;
        auto backend = gpuroute::create_cpu_backend();
        if (!backend->init(g, 4, 12, 4, 20, error)) {
            std::fprintf(stderr, "backend init failed: %s\n", error.c_str());
            std::exit(2);
        }
        gpuroute::RouteParams params;
        params.use_bb = 0;
        params.exact = 1;
        params.expand_k = 1;
        params.bias_factor = 0.0f;
        gpuroute::TaskDesc task;
        task.tree_cnt = 1;
        task.arc_cnt = 1;
        task.path_cap = 64;
        task.hpwl = 6;
        task.fanout = 1;
        task.stay_in_halo = stay;
        task.shell_cnt = 1;
        std::vector<int32_t> packed{gpuroute::pack_shell_tile(6, 0)};
        std::vector<gpuroute::ArcResult> results;
        std::vector<gpuroute::PathEntry> paths;
        backend->route(params, {task}, {{3, 1.0f, 0.0f}}, {0}, {0.0f}, {0.0f}, packed, false, results, paths);
        return results;
    };
    auto blocked = only(1);
    if (blocked.size() != 1 || blocked[0].status != gpuroute::ARC_NO_PATH) {
        std::fprintf(stderr, "only-outside route was proposed: status=%d\n",
                     blocked.empty() ? -1 : blocked[0].status);
        return 1;
    }
    auto open_only = only(0);
    if (open_only.size() != 1 || open_only[0].status != gpuroute::ARC_OK) {
        std::fprintf(stderr, "only-outside route did not route with the fence off: status=%d\n",
                     open_only.empty() ? -1 : open_only[0].status);
        return 1;
    }
    std::puts("GPU router halo-shell gate regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="gpurouter-halo-shell-") as tmp:
    tmp = Path(tmp)
    cpp = tmp / "halo_shell.cc"
    exe = tmp / "halo_shell"
    cpp.write_text(DRIVER)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "common/route/gpu"), str(cpp), str(CPU_BACKEND),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
