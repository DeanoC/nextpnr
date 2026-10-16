#!/usr/bin/env python3
"""A task that may not enter a reserved region must not be offered its pips.

No device or chip database needed: the sequential host backend is a scalar
transcription of the device kernel, so driving it with a two-path graph shows
both that the reserved edge is refused and that it is still taken by a task
that is allowed to use it. Without the edge_region gate the search prefers the
cheaper reserved edge and the Arch rejects the binding afterwards, which is the
re-route loop of DeanoC/nextpnr#195.
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
#include <vector>

#include "gpuroute_backend.h"

namespace {

// Two routes from wire 0 to wire 3:
//   0 -> 1 -> 3   legal, 2.0 + 1.0      cost 3.0
//   0 -> 2 -> 3   wire 2 is reserved, 1.0 + 1.0  cost 2.0
const int32_t out_off[] = {0, 2, 3, 4, 4};
const int32_t out_dst[] = {1, 2, 3, 3};
const float edge_cost[] = {2.0f, 1.0f, 1.0f, 1.0f};
const uint8_t edge_region[] = {0, 1, 0, 0};
const int16_t wire_x[] = {0, 1, 1, 2};
const int16_t wire_y[] = {0, 0, 1, 0};
const int32_t wire_reserved[] = {-1, -1, -1, -1};
const uint8_t wire_flags[] = {0, 0, 0, 0};
const int32_t wire_occ[] = {0, 0, 0, 0};
const float wire_hist[] = {1.0f, 1.0f, 1.0f, 1.0f};

bool path_uses_wire(int32_t wire, const uint8_t *region = edge_region)
{
    std::string error;
    gpuroute::GraphData g;
    g.n_wires = 4;
    g.n_edges = 4;
    g.out_off = out_off;
    g.out_dst = out_dst;
    g.edge_cost = edge_cost;
    g.edge_region = region;
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
    params.use_bb = 0;   // both routes stay inside the box; keep the search general
    params.exact = 1;    // plain A*: deterministic single lowest-f expansion
    params.expand_k = 1;
    params.bias_factor = 0.0f;

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
    task.hpwl = 2;
    task.fanout = 1;
    task.allow_reserved = g_allow_reserved;

    std::vector<gpuroute::TaskDesc> tasks{task};
    std::vector<gpuroute::ArcDesc> arcs{{3, 1.0f, 0.0f}};
    std::vector<int32_t> seeds{0};
    std::vector<float> seed_delay{0.0f};
    std::vector<float> seed_load{0.0f};
    std::vector<gpuroute::ArcResult> results;
    std::vector<gpuroute::PathEntry> paths;

    backend->route(params, tasks, arcs, seeds, seed_delay, seed_load, false, results, paths);
    assert(results.size() == 1);
    if (results[0].status != gpuroute::ARC_OK) {
        std::fprintf(stderr, "arc not routed: status=%d\n", results[0].status);
        std::exit(3);
    }
    for (int32_t i = 0; i < results[0].path_len; i++) {
        const auto &e = paths.at(results[0].path_off + i);
        if (e.wire == wire || e.parent == wire)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    // A task that may not enter the reserved region must take the legal route.
    g_allow_reserved = 0;
    if (path_uses_wire(2)) {
        std::fprintf(stderr, "reserved-region edge was offered to a task with allow_reserved=0\n");
        return 1;
    }
    if (!path_uses_wire(1)) {
        std::fprintf(stderr, "legal route was not taken\n");
        return 1;
    }
    // A task that may enter it still prefers the cheaper reserved edge.
    g_allow_reserved = 1;
    if (!path_uses_wire(2)) {
        std::fprintf(stderr, "allow_reserved=1 did not take the cheaper reserved edge\n");
        return 1;
    }
    // A null region is "no reserved edges": allow_reserved=0 must still take
    // the cheaper edge, and must not crash on the load.
    g_allow_reserved = 0;
    if (!path_uses_wire(2, nullptr)) {
        std::fprintf(stderr, "null edge_region was treated as reserved\n");
        return 1;
    }
    std::puts("GPU router reserved-region gate regression passed");
    return 0;
}
'''

SOURCE_PREFIX = "int g_allow_reserved = 1;\n"

with tempfile.TemporaryDirectory(prefix="gpurouter-reserved-region-") as tmp:
    tmp = Path(tmp)
    cpp = tmp / "reserved_region.cc"
    exe = tmp / "reserved_region"
    cpp.write_text(SOURCE_PREFIX + DRIVER)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "common/route/gpu"), str(cpp), str(CPU_BACKEND),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
