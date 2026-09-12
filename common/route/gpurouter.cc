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
 *
 *  Host side: flattens the Arch routing graph, keeps the authoritative
 *  per-net routing trees and per-wire congestion state, decides which
 *  connections to rip up and re-route each iteration, groups the affected
 *  nets into batches whose bounding boxes do not overlap, and hands each
 *  batch to the device backend (common/route/gpu/). Paths coming back are
 *  applied in a fixed order so the result does not depend on GPU scheduling.
 *
 *  The cost model (base delay * history * present congestion, criticality
 *  weighting, centroid bias) and the congestion schedule follow router2 /
 *  CRoute so that results are comparable, the difference being where and how
 *  the shortest-path searches run. See docs/gpurouter.md.
 */

#include "gpurouter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "gpu/gpuroute_backend.h"
#include "log.h"
#include "nextpnr.h"
#include "nextpnr_assertions.h"
#include "router1.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

using Clock = std::chrono::steady_clock;
inline double secs_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct GpuRouter
{
    Context *ctx;
    GpuRouterCfg cfg;
    TimingAnalyser tmg;
    std::unique_ptr<gpuroute::Backend> backend;     // device (or CPU fallback)
    std::unique_ptr<gpuroute::Backend> cpu_backend; // CPU lane for tiny batches, if distinct

    bool timing_driven = false, timing_driven_ripup = false;
    float curr_cong_weight = 0.5f, hist_cong_weight = 1.0f;

    GpuRouter(Context *ctx, const GpuRouterCfg &cfg) : ctx(ctx), cfg(cfg), tmg(ctx)
    {
        tmg.setup_only = false;
        tmg.with_clock_skew = true;
        tmg.setup();
    }

    // ------------------------------------------------------------------
    // Flattened routing graph

    std::vector<WireId> idx_to_wire;
    dict<WireId, int32_t> wire_to_idx;
    std::vector<int32_t> out_off, out_dst;
    std::vector<float> edge_cost;
    std::vector<int16_t> wx, wy;
    std::vector<int32_t> reserved;
    std::vector<uint8_t> wflags;
    std::vector<int32_t> occ;
    std::vector<float> hist;

    std::vector<int32_t> dirty_wires;
    std::vector<uint8_t> dirty_mark;

    void mark_dirty(int32_t w)
    {
        if (!dirty_mark[w]) {
            dirty_mark[w] = 1;
            dirty_wires.push_back(w);
        }
    }

    int32_t widx(WireId w) const
    {
        auto fnd = wire_to_idx.find(w);
        NPNR_ASSERT(fnd != wire_to_idx.end());
        return fnd->second;
    }

    // ------------------------------------------------------------------
    // Per-net data

    struct TreeWire
    {
        int32_t parent = -1; // driving wire, -1 for the source / a pre-bound root
        PipId pip;           // pip parent -> wire
        int32_t count = 0;   // arcs of this net using the wire
        float delay = 0.0f;  // base delay from the source along the tree (ns)
    };

    struct ArcData
    {
        int32_t sink = -1;
        BoundingBox bb;
        bool routed = false, pre_routed = false;
        bool frozen = false; // repaired for timing; its wires are reserved and it is never ripped up
    };

    struct NetData
    {
        int32_t src = -1;
        dict<int32_t, TreeWire> wires;
        std::vector<std::vector<ArcData>> arcs;
        BoundingBox bb;
        int cx = 0, cy = 0, hpwl = 1;
        int fail_count = 0;
        float max_crit = 0.0f;
    };

    std::vector<NetInfo *> nets_by_udata;
    std::vector<NetData> nets;

    int num_threads() const
    {
        int n = int(std::thread::hardware_concurrency());
        if (n <= 0)
            n = 4;
        if (ctx->settings.count(ctx->id("threads")))
            n = std::max(1, ctx->setting<int>("threads"));
        return std::min(n, 32);
    }

    template <typename F> void parallel_chunks(size_t count, F fn)
    {
#ifdef NPNR_DISABLE_THREADS
        // e.g. the WASI build: pthread_create() always fails there
        fn(size_t(0), count);
        return;
#endif
        int nt = std::min<size_t>(num_threads(), std::max<size_t>(1, count / 4096));
        if (nt <= 1) {
            fn(size_t(0), count);
            return;
        }
        std::vector<std::thread> threads;
        size_t chunk = (count + nt - 1) / nt;
        for (int t = 0; t < nt; t++) {
            size_t b = std::min(count, size_t(t) * chunk), e = std::min(count, size_t(t + 1) * chunk);
            if (b >= e)
                break;
            threads.emplace_back([=]() { fn(b, e); });
        }
        for (auto &t : threads)
            t.join();
    }

    // ------------------------------------------------------------------
    // Setup

    void setup_net_indices()
    {
        nets.resize(ctx->nets.size());
        nets_by_udata.resize(ctx->nets.size());
        size_t i = 0;
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            if (ni->constant_value != IdString())
                log_error("The GPU router does not support constant-value nets (net '%s').\n", ctx->nameOf(ni));
            ni->udata = int32_t(i);
            nets_by_udata.at(i) = ni;
            i++;
        }
    }

    void setup_graph()
    {
        auto t0 = Clock::now();
        struct Tmp
        {
            WireId w;
            int16_t x, y;
        };
        std::vector<Tmp> tmp;
        for (auto w : ctx->getWires()) {
            BoundingBox b = ctx->getRouteBoundingBox(w, w);
            tmp.push_back(Tmp{w, int16_t((b.x0 + b.x1) / 2), int16_t((b.y0 + b.y1) / 2)});
        }
        // Tile-major numbering keeps the wires of a region close in memory
        std::stable_sort(tmp.begin(), tmp.end(), [](const Tmp &a, const Tmp &b) {
            if (a.y != b.y)
                return a.y < b.y;
            return a.x < b.x;
        });
        const size_t n = tmp.size();
        idx_to_wire.resize(n);
        wx.resize(n);
        wy.resize(n);
        for (size_t i = 0; i < n; i++) {
            idx_to_wire[i] = tmp[i].w;
            wx[i] = tmp[i].x;
            wy[i] = tmp[i].y;
            wire_to_idx[tmp[i].w] = int32_t(i);
        }
        tmp.clear();
        tmp.shrink_to_fit();

        // CSR adjacency. Counting and filling are independent per wire and
        // only use const Arch queries, so they run on several threads.
        out_off.assign(n + 1, 0);
        parallel_chunks(n, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; i++) {
                int32_t cnt = 0;
                for (auto pip : ctx->getPipsDownhill(idx_to_wire[i])) {
                    (void)pip;
                    cnt++;
                }
                out_off[i + 1] = cnt;
            }
        });
        for (size_t i = 0; i < n; i++)
            out_off[i + 1] += out_off[i];
        const int64_t m = out_off[n];
        out_dst.resize(m);
        edge_cost.resize(m);
        const delay_t eps = ctx->getDelayEpsilon();
        parallel_chunks(n, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; i++) {
                int32_t k = out_off[i];
                for (auto pip : ctx->getPipsDownhill(idx_to_wire[i])) {
                    WireId dst = ctx->getPipDstWire(pip);
                    out_dst[k] = widx(dst);
                    float c = ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(dst).maxDelay() + eps);
                    if (!ctx->checkPipAvail(pip) && ctx->getBoundPipNet(pip) == nullptr)
                        c = -1.0f; // blocked by an architecture rule
                    else if (c < 1e-6f)
                        c = 1e-6f;
                    edge_cost[k] = c;
                    k++;
                }
            }
        });

        // Per-wire state, mirroring what is already bound in the Arch
        reserved.assign(n, -1);
        wflags.assign(n, 0);
        occ.assign(n, 0);
        hist.assign(n, 1.0f);
        dirty_mark.assign(n, 0);
        for (size_t i = 0; i < n; i++) {
            WireId w = idx_to_wire[i];
            NetInfo *bound = ctx->getBoundWireNet(w);
            if (bound == nullptr)
                continue;
            auto iter = bound->wires.find(w);
            if (iter == bound->wires.end())
                continue;
            auto &nd = nets.at(bound->udata);
            TreeWire tw;
            tw.pip = iter->second.pip;
            tw.parent = (tw.pip == PipId()) ? -1 : widx(ctx->getPipSrcWire(tw.pip));
            tw.count = 0;
            nd.wires[int32_t(i)] = tw;
            occ[i] = 1;
            if (iter->second.strength == STRENGTH_PLACER)
                reserved[i] = bound->udata;
            else if (iter->second.strength > STRENGTH_PLACER)
                wflags[i] |= gpuroute::WIRE_UNAVAILABLE;
        }
        log_info("    flattened %zu wires and %lld pips in %.2fs\n", n, (long long)m, secs_since(t0));
    }

    void setup_nets()
    {
        for (size_t i = 0; i < nets.size(); i++) {
            NetInfo *ni = nets_by_udata.at(i);
            auto &nd = nets.at(i);
            nd.arcs.resize(ni->users.capacity());
            nd.bb.x0 = std::numeric_limits<int>::max();
            nd.bb.x1 = std::numeric_limits<int>::min();
            nd.bb.y0 = std::numeric_limits<int>::max();
            nd.bb.y1 = std::numeric_limits<int>::min();
            nd.cx = 0;
            nd.cy = 0;
            if (ni->driver.cell == nullptr)
                continue;
            Loc drv_loc = ni->driver.cell->getLocation();
            nd.cx += drv_loc.x;
            nd.cy += drv_loc.y;
            WireId src_wire = ctx->getNetinfoSourceWire(ni);
            for (auto usr : ni->users.enumerate()) {
                for (auto &dst_wire : ctx->getNetinfoSinkWires(ni, usr.value)) {
                    if (src_wire == WireId())
                        log_error("No wire found for port %s on source cell %s.\n", ctx->nameOf(ni->driver.port),
                                  ctx->nameOf(ni->driver.cell));
                    if (dst_wire == WireId())
                        log_error("No wire found for port %s on destination cell %s.\n", ctx->nameOf(usr.value.port),
                                  ctx->nameOf(usr.value.cell));
                    nd.arcs.at(usr.index.idx()).emplace_back();
                    auto &ad = nd.arcs.at(usr.index.idx()).back();
                    ad.sink = widx(dst_wire);
                    ad.bb = ctx->getRouteBoundingBox(src_wire, dst_wire);
                    nd.bb.x0 = std::min(nd.bb.x0, ad.bb.x0);
                    nd.bb.x1 = std::max(nd.bb.x1, ad.bb.x1);
                    nd.bb.y0 = std::min(nd.bb.y0, ad.bb.y0);
                    nd.bb.y1 = std::max(nd.bb.y1, ad.bb.y1);
                }
                Loc usr_loc = usr.value.cell->getLocation();
                nd.cx += usr_loc.x;
                nd.cy += usr_loc.y;
            }
            if (src_wire != WireId()) {
                nd.src = widx(src_wire);
                if (!nd.wires.count(nd.src)) {
                    TreeWire tw;
                    tw.parent = -1;
                    tw.count = 1;
                    nd.wires[nd.src] = tw;
                    occ[nd.src]++;
                }
            }
            nd.hpwl = std::max(std::abs(nd.bb.y1 - nd.bb.y0) + std::abs(nd.bb.x1 - nd.bb.x0), 1);
            nd.cx /= int(ni->users.entries() + 1);
            nd.cy /= int(ni->users.entries() + 1);
            nd.bb.x0 = std::max(nd.bb.x0 - cfg.bb_margin_x, 0);
            nd.bb.y0 = std::max(nd.bb.y0 - cfg.bb_margin_y, 0);
            nd.bb.x1 = std::min(nd.bb.x1 + cfg.bb_margin_x, ctx->getGridDimX());
            nd.bb.y1 = std::min(nd.bb.y1 + cfg.bb_margin_y, ctx->getGridDimY());
        }
        // Pre-routed arcs (e.g. dedicated global clock routing)
        for (size_t i = 0; i < nets.size(); i++) {
            auto &nd = nets.at(i);
            compute_tree_delays(nd);
            for (auto &arcs : nd.arcs)
                for (auto &ad : arcs)
                    if (check_arc_routing(nd, ad))
                        record_prerouted(nd, ad);
        }
    }

    // ------------------------------------------------------------------
    // Tree bookkeeping

    float pip_base_cost(PipId pip, int32_t wire) const
    {
        if (pip == PipId())
            return 0.0f;
        return ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(idx_to_wire[wire]).maxDelay() +
                               ctx->getDelayEpsilon());
    }

    void bind_internal(NetData &nd, int32_t wire, int32_t parent, PipId pip)
    {
        auto fnd = nd.wires.find(wire);
        if (fnd == nd.wires.end()) {
            TreeWire tw;
            tw.parent = parent;
            tw.pip = pip;
            tw.count = 1;
            if (parent >= 0) {
                auto pf = nd.wires.find(parent);
                tw.delay = (pf == nd.wires.end() ? 0.0f : pf->second.delay) + pip_base_cost(pip, wire);
            }
            nd.wires[wire] = tw;
            occ[wire]++;
            mark_dirty(wire);
        } else {
            NPNR_ASSERT(fnd->second.parent == parent);
            fnd->second.count++;
        }
    }

    // Upstream delays of wires that were bound before the router ran
    void compute_tree_delays(NetData &nd)
    {
        std::vector<int32_t> chain;
        for (auto &w : nd.wires) {
            chain.clear();
            int32_t cursor = w.first;
            while (true) {
                auto fnd = nd.wires.find(cursor);
                if (fnd == nd.wires.end() || fnd->second.parent == -1 || fnd->second.delay > 0.0f)
                    break;
                chain.push_back(cursor);
                cursor = fnd->second.parent;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                auto &tw = nd.wires.at(*it);
                auto pf = nd.wires.find(tw.parent);
                tw.delay = (pf == nd.wires.end() ? 0.0f : pf->second.delay) + pip_base_cost(tw.pip, *it);
            }
        }
    }

    void unbind_internal(NetData &nd, int32_t wire)
    {
        auto fnd = nd.wires.find(wire);
        NPNR_ASSERT(fnd != nd.wires.end());
        if (--fnd->second.count <= 0) {
            nd.wires.erase(fnd);
            occ[wire]--;
            mark_dirty(wire);
        }
    }

    bool check_arc_routing(const NetData &nd, const ArcData &ad) const
    {
        if (nd.src < 0)
            return false;
        int32_t cursor = ad.sink;
        while (true) {
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end())
                return false;
            if (occ[cursor] != 1)
                return false;
            if (fnd->second.parent == -1)
                break;
            cursor = fnd->second.parent;
        }
        return cursor == nd.src;
    }

    void record_prerouted(NetData &nd, ArcData &ad)
    {
        ad.routed = true;
        ad.pre_routed = true;
        int32_t cursor = ad.sink;
        while (cursor != nd.src) {
            TreeWire tw = nd.wires.at(cursor);
            bind_internal(nd, cursor, tw.parent, tw.pip);
            if (tw.parent == -1)
                break;
            cursor = tw.parent;
        }
    }

    void ripup_arc(NetData &nd, ArcData &ad)
    {
        if (!ad.routed)
            return;
        int32_t cursor = ad.sink;
        while (cursor != nd.src) {
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end())
                break;
            int32_t parent = fnd->second.parent;
            unbind_internal(nd, cursor);
            if (parent == -1)
                break;
            cursor = parent;
        }
        ad.routed = false;
        ad.pre_routed = false;
    }

    // The pip behind CSR edge `edge`: the (edge - out_off[parent])-th downhill
    // pip of the parent wire, in the same enumeration order the graph was
    // built from, so parallel pips between one wire pair stay distinct.
    PipId pip_for_edge(int32_t parent, int32_t wire, int32_t edge) const
    {
        WireId pw = idx_to_wire.at(parent), dw = idx_to_wire.at(wire);
        if (edge >= out_off.at(parent) && edge < out_off.at(parent + 1)) {
            int32_t k = edge - out_off.at(parent);
            for (auto pip : ctx->getPipsDownhill(pw)) {
                if (k-- == 0) {
                    NPNR_ASSERT(ctx->getPipDstWire(pip) == dw);
                    return pip;
                }
            }
        }
        log_error("Internal error: edge %d is not a pip from %s to %s.\n", edge, ctx->nameOfWire(pw),
                  ctx->nameOfWire(dw));
    }

    void apply_path(NetData &nd, ArcData &ad, const gpuroute::PathEntry *entries, int len)
    {
        NPNR_ASSERT(len > 0);
        // attach point first so upstream delays are known when binding
        for (int k = len - 1; k >= 0; k--) {
            int32_t w = entries[k].wire, parent = entries[k].parent;
            NPNR_ASSERT(parent >= 0);
            bind_internal(nd, w, parent, pip_for_edge(parent, w, entries[k].edge));
        }
        // The arc also uses the existing tree from the attach point up to
        // the source; count it there too so shared wires survive rip-ups
        // of sibling arcs.
        int32_t cursor = entries[len - 1].parent;
        while (true) {
            auto fnd = nd.wires.find(cursor);
            NPNR_ASSERT(fnd != nd.wires.end());
            fnd->second.count++;
            if (fnd->second.parent == -1 || cursor == nd.src)
                break;
            cursor = fnd->second.parent;
        }
        ad.routed = true;
    }

    delay_t get_route_delay(const NetData &nd, const ArcData &ad) const
    {
        if (nd.src < 0 || !nd.wires.count(ad.sink))
            return 0;
        delay_t delay = 0;
        int32_t cursor = ad.sink;
        while (true) {
            delay += ctx->getWireDelay(idx_to_wire[cursor]).maxDelay();
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end())
                break;
            if (fnd->second.pip == PipId())
                break;
            delay += ctx->getPipDelay(fnd->second.pip).maxDelay();
            cursor = fnd->second.parent;
        }
        return delay;
    }

    // ------------------------------------------------------------------
    // Wire reservation (ported from router2): wires on a single path from a
    // driver or to a sink can only ever be used by that net.

    bool is_wire_unusable(WireId wire, const NetInfo *net, const pool<WireId> &sink_wires, int iter_count = 0)
    {
        if (iter_count > 7)
            return false;
        int32_t w = widx(wire);
        if (reserved[w] != -1 && reserved[w] != net->udata)
            return true;
        if (sink_wires.count(wire))
            return false;
        for (auto p : ctx->getPipsDownhill(wire))
            if (ctx->checkPipAvail(p))
                if (!is_wire_unusable(ctx->getPipDstWire(p), net, sink_wires, iter_count + 1))
                    return false;
        return true;
    }

    bool is_wire_undriveable(WireId wire, const NetInfo *net, int iter_count = 0)
    {
        if (iter_count > 7)
            return false;
        int32_t w = widx(wire);
        if (wflags[w] & gpuroute::WIRE_UNAVAILABLE)
            return true;
        if (reserved[w] != -1 && reserved[w] != net->udata)
            return true;
        for (auto bp : ctx->getWireBelPins(wire))
            if ((net->driver.cell == nullptr || bp.bel == net->driver.cell->bel) &&
                ctx->getBelPinType(bp.bel, bp.pin) != PORT_IN)
                return false;
        for (auto p : ctx->getPipsUphill(wire))
            if (ctx->checkPipAvail(p))
                if (!is_wire_undriveable(ctx->getPipSrcWire(p), net, iter_count + 1))
                    return false;
        return true;
    }

    bool reserve_driver_wires(NetInfo *net, const pool<WireId> &sink_wires)
    {
        bool did_something = false;
        WireId src = ctx->getNetinfoSourceWire(net);
        if (src == WireId() || sink_wires.count(src) || sink_wires.empty())
            return false;
        WireId cursor = src;
        bool done = false;
        while (!done) {
            int32_t c = widx(cursor);
            did_something |= (reserved[c] != net->udata);
            if (reserved[c] != -1 && reserved[c] != net->udata)
                log_error("attempting to reserve driver output path wire '%s' for nets '%s' and '%s'\n",
                          ctx->nameOfWire(cursor), ctx->nameOf(nets_by_udata.at(reserved[c])), ctx->nameOf(net));
            reserved[c] = net->udata;
            WireId next_cursor;
            for (auto dh : ctx->getPipsDownhill(cursor)) {
                WireId w = ctx->getPipDstWire(dh);
                if (is_wire_unusable(w, net, sink_wires))
                    continue;
                if (next_cursor != WireId() || sink_wires.count(w)) {
                    done = true;
                    break;
                }
                next_cursor = w;
            }
            if (next_cursor == WireId())
                break;
            cursor = next_cursor;
        }
        return did_something;
    }

    bool reserve_sink_wires(NetInfo *net, store_index<PortRef> i)
    {
        bool did_something = false;
        WireId src = ctx->getNetinfoSourceWire(net);
        auto &nd = nets.at(net->udata);
        for (auto &ad : nd.arcs.at(i.idx())) {
            if (ad.pre_routed)
                continue;
            WireId cursor = idx_to_wire[ad.sink];
            bool done = false;
            while (!done) {
                int32_t c = widx(cursor);
                did_something |= (reserved[c] != net->udata);
                if (reserved[c] != -1 && reserved[c] != net->udata)
                    log_error("attempting to reserve sink input path wire '%s' for nets '%s' and '%s'\n",
                              ctx->nameOfWire(cursor), ctx->nameOf(nets_by_udata.at(reserved[c])), ctx->nameOf(net));
                reserved[c] = net->udata;
                if (cursor == src)
                    break;
                WireId next_cursor;
                for (auto uh : ctx->getPipsUphill(cursor)) {
                    WireId w = ctx->getPipSrcWire(uh);
                    if (is_wire_undriveable(w, net))
                        continue;
                    if (next_cursor != WireId()) {
                        done = true;
                        break;
                    }
                    next_cursor = w;
                }
                if (next_cursor == WireId())
                    break;
                cursor = next_cursor;
            }
        }
        return did_something;
    }

    void find_all_reserved_wires()
    {
        bool did_something;
        do {
            did_something = false;
            for (auto net : nets_by_udata) {
                auto &nd = nets.at(net->udata);
                if (net->driver.cell == nullptr || nd.src < 0)
                    continue;
                pool<WireId> sink_wires;
                bool all_pre_routed = true;
                for (auto usr : net->users.enumerate()) {
                    for (auto &sink : nd.arcs.at(usr.index.idx()))
                        if (!sink.pre_routed)
                            all_pre_routed = false;
                    for (auto sink_wire : ctx->getNetinfoSinkWires(net, usr.value))
                        sink_wires.insert(sink_wire);
                }
                if (all_pre_routed)
                    continue;
                did_something |= reserve_driver_wires(net, sink_wires);
                for (auto usr : net->users.enumerate())
                    did_something |= reserve_sink_wires(net, usr.index);
            }
        } while (did_something);
    }

    // ------------------------------------------------------------------
    // Timing helpers

    // Arcs that fail slack under timing-driven rip-up are treated as fully
    // critical so they take the minimum-delay route and others move.
    float arc_crit(NetInfo *net, store_index<PortRef> i)
    {
        if (!timing_driven)
            return 0.0f;
        if (arc_failed_slack(net, i))
            return 1.0f;
        return tmg.get_criticality(CellPortKey(net->users.at(i)));
    }

    float arc_crit_weight(NetInfo *net, store_index<PortRef> i)
    {
        if (!timing_driven)
            return 1.0f;
        float crit = arc_crit(net, i);
        float w = cfg.crit_weight_mode == 0 ? 1.0f - std::pow(crit, 2) : std::pow(1.0f - crit, cfg.crit_exponent);
        return std::max<float>(cfg.crit_weight_floor, w);
    }

    bool arc_failed_slack(NetInfo *net, store_index<PortRef> i)
    {
        return timing_driven_ripup && (tmg.get_setup_slack(CellPortKey(net->users.at(i))) < (2 * ctx->getDelayEpsilon()));
    }
    int best_tmgfail = std::numeric_limits<int>::max(), tmgfail_stall = 0;

    void update_route_delays(const std::vector<int> &queue)
    {
        for (int n : queue) {
            NetInfo *ni = nets_by_udata.at(n);
            auto &nd = nets.at(n);
            for (auto usr : ni->users.enumerate()) {
                delay_t arc_delay = 0;
                for (auto &ad : nd.arcs.at(usr.index.idx()))
                    arc_delay = std::max(arc_delay, get_route_delay(nd, ad));
                tmg.set_route_delay(CellPortKey(usr.value), DelayPair(arc_delay));
            }
        }
    }

    // ------------------------------------------------------------------
    // Estimate model: fit a*|dx| + b*|dy| to Arch::estimateDelay so the
    // device heuristic matches the architecture without calling into it.

    float est_x = 0.075f, est_y = 0.2f;

    void fit_estimate()
    {
        if (idx_to_wire.size() < 2)
            return;
        DeterministicRNG rng;
        rng.rngseed(0x5eed1234u);
        double sxx = 0, sxy = 0, syy = 0, sxd = 0, syd = 0, sd = 0, sxy_sum = 0;
        int samples = 0;
        for (int i = 0; i < 4000; i++) {
            int a = rng.rng(int(idx_to_wire.size())), b = rng.rng(int(idx_to_wire.size()));
            double dx = std::abs(wx[a] - wx[b]), dy = std::abs(wy[a] - wy[b]);
            if (dx + dy == 0)
                continue;
            double d = ctx->getDelayNS(ctx->estimateDelay(idx_to_wire[a], idx_to_wire[b]));
            sxx += dx * dx;
            sxy += dx * dy;
            syy += dy * dy;
            sxd += dx * d;
            syd += dy * d;
            sd += d;
            sxy_sum += dx + dy;
            samples++;
        }
        if (samples == 0)
            return;
        double det = sxx * syy - sxy * sxy;
        if (std::abs(det) > 1e-9) {
            double a = (sxd * syy - syd * sxy) / det;
            double b = (sxx * syd - sxy * sxd) / det;
            if (a >= 0 && b >= 0 && (a > 0 || b > 0)) {
                est_x = float(a);
                est_y = float(b);
                return;
            }
        }
        float avg = float(sd / std::max(1.0, sxy_sum));
        est_x = est_y = avg;
    }

    // ------------------------------------------------------------------
    // Routing iteration

    struct HostTask
    {
        int net;
        std::vector<std::pair<int, int>> arcs; // (user index, phys pin)
        int path_scale = 1;                    // grows when a path did not fit its output region
    };

    std::vector<int> route_queue;
    std::set<int> failed_nets;
    int total_wire_use = 0, overused_wires = 0, total_overuse = 0, arch_fail = 0;
    double gpu_time = 0.0;
    int64_t last_arcs = 0, last_expanded = 0;

    void flush_state()
    {
        if (dirty_wires.empty())
            return;
        std::vector<int32_t> o(dirty_wires.size()), r(dirty_wires.size());
        std::vector<float> h(dirty_wires.size());
        for (size_t i = 0; i < dirty_wires.size(); i++) {
            o[i] = occ[dirty_wires[i]];
            h[i] = hist[dirty_wires[i]];
            r[i] = reserved[dirty_wires[i]];
            dirty_mark[dirty_wires[i]] = 0;
        }
        backend->update_wire_state(dirty_wires.size(), dirty_wires.data(), o.data(), h.data(), r.data());
        if (cpu_backend)
            cpu_backend->update_wire_state(dirty_wires.size(), dirty_wires.data(), o.data(), h.data(), r.data());
        dirty_wires.clear();
    }

    // Tiny batches (the negotiation tail, one-net repairs) are cheaper on
    // the host than as a one-block launch plus table clears on the device
    gpuroute::Backend *backend_for(size_t ntasks)
    {
        if (cpu_backend && int(ntasks) <= cfg.cpu_lane_nets)
            return cpu_backend.get();
        return backend.get();
    }

    // Repair displacement: soft reservations of other nets are ignored
    bool ignore_soft = false;

    // Timing repair mode: pure delay, no congestion terms, no bias
    bool repair_mode = false;

    gpuroute::RouteParams make_params(bool use_bb) const
    {
        gpuroute::RouteParams p;
        p.est_x = est_x;
        p.est_y = est_y;
        p.est_weight = cfg.estimate_weight;
        p.curr_cong_weight = curr_cong_weight;
        p.bias_factor = repair_mode ? 0.0f : cfg.bias_cost_factor;
        p.seed_delay_weight = cfg.seed_delay_weight;
        p.seed_delay_floor = cfg.seed_delay_floor;
        p.load_penalty = cfg.load_penalty;
        p.pip_adder = cfg.pip_adder;
        p.expand_k = cfg.expand_k;
        p.expand_div = cfg.expand_div;
        p.use_bb = use_bb ? 1 : 0;
        p.ignore_soft = ignore_soft ? 1 : 0;
        p.max_probe = 512;
        return p;
    }

    // Route the given tasks in one backend call. Arcs that could not be
    // routed are returned for a retry.
    int last_fail_status = 0, last_fail_reason = 0, last_fail_expanded = 0;
    std::vector<HostTask> route_tasks(const std::vector<HostTask> &tasks, bool use_bb, bool large_lane)
    {
        std::vector<gpuroute::TaskDesc> tds;
        std::vector<gpuroute::ArcDesc> ads;
        std::vector<int32_t> seeds;
        std::vector<float> seed_delay, seed_load;
        std::vector<std::vector<std::pair<int, int>>> arc_map(tasks.size());
        dict<int32_t, int> children;
        int path_cursor = 0;
        for (size_t ti = 0; ti < tasks.size(); ti++) {
            const HostTask &t = tasks[ti];
            NetInfo *ni = nets_by_udata.at(t.net);
            auto &nd = nets.at(t.net);
            gpuroute::TaskDesc td;
            td.net = t.net;
            td.tree_off = int32_t(seeds.size());
            children.clear();
            for (auto &w : nd.wires)
                if (w.second.parent >= 0)
                    children[w.second.parent]++;
            // In repair mode the new arc will be frozen together with its
            // upstream tree, so it may only attach where that upstream path
            // is free of wires reserved for other nets.
            dict<int32_t, bool> clean;
            std::function<bool(int32_t)> is_clean = [&](int32_t w) -> bool {
                auto fnd = clean.find(w);
                if (fnd != clean.end())
                    return fnd->second;
                bool ok = (reserved[w] == -1 || gpuroute::reserved_owner(reserved[w]) == t.net);
                auto tw = nd.wires.find(w);
                if (ok && tw != nd.wires.end() && tw->second.parent >= 0 && w != nd.src)
                    ok = is_clean(tw->second.parent);
                clean[w] = ok;
                return ok;
            };
            int clean_count = 0;
            for (auto &w : nd.wires) {
                seeds.push_back(w.first);
                auto ch = children.find(w.first);
                seed_load.push_back(ch == children.end() ? 0.0f : float(ch->second));
                if (repair_mode && !is_clean(w.first)) {
                    // still part of the tree (keeps its driver) but not an
                    // attach point and not passable in this search
                    seed_delay.push_back(-1.0f);
                } else {
                    seed_delay.push_back(w.second.delay);
                    clean_count++;
                }
            }
            if (clean_count == 0) {
                // nothing clean to attach to: allow the source itself
                seed_delay[td.tree_off + int32_t(std::distance(nd.wires.begin(), nd.wires.find(nd.src)))] = 0.0f;
            }
            td.tree_cnt = int32_t(seeds.size()) - td.tree_off;
            td.arc_off = int32_t(ads.size());
            pool<int32_t> sinks_seen;
            for (auto &a : t.arcs) {
                auto &ad = nd.arcs.at(a.first).at(a.second);
                // Already connected through another logical arc of this net
                if (nd.wires.count(ad.sink) || sinks_seen.count(ad.sink))
                    continue;
                sinks_seen.insert(ad.sink);
                gpuroute::ArcDesc d;
                d.sink = ad.sink;
                d.crit_weight = repair_mode ? 0.0f : arc_crit_weight(ni, store_index<PortRef>(a.first));
                d.crit = repair_mode ? 1.0f : arc_crit(ni, store_index<PortRef>(a.first));
                ads.push_back(d);
                arc_map[ti].push_back(a);
            }
            td.arc_cnt = int32_t(ads.size()) - td.arc_off;
            td.path_off = path_cursor;
            // Output region for the new wires of this task's arcs. A route
            // that does not fit returns ARC_PATH_FULL and is retried with
            // the region scaled up (bounded by the wire count, which no
            // simple path can exceed).
            int64_t cap = int64_t(96 * td.arc_cnt + 256) * t.path_scale;
            cap = std::min<int64_t>(cap, int64_t(idx_to_wire.size()) * std::max(1, td.arc_cnt));
            td.path_cap = int32_t(cap);
            path_cursor += td.path_cap;
            td.cx = nd.cx;
            td.cy = nd.cy;
            td.hpwl = std::max(nd.hpwl, 1);
            td.fanout = std::max<int>(1, int(ni->users.entries()));
            td.bb_x0 = int16_t(std::max(nd.bb.x0, 0));
            td.bb_y0 = int16_t(std::max(nd.bb.y0, 0));
            td.bb_x1 = int16_t(std::min(nd.bb.x1, ctx->getGridDimX()));
            td.bb_y1 = int16_t(std::min(nd.bb.y1, ctx->getGridDimY()));
            tds.push_back(td);
        }
        std::vector<gpuroute::ArcResult> results;
        std::vector<gpuroute::PathEntry> paths;
        auto t0 = Clock::now();
        backend_for(tasks.size())->route(make_params(use_bb), tds, ads, seeds, seed_delay, seed_load, large_lane,
                                         results, paths);
        gpu_time += secs_since(t0);

        std::vector<HostTask> retry;
        for (size_t ti = 0; ti < tasks.size(); ti++) {
            const auto &td = tds[ti];
            auto &nd = nets.at(td.net);
            HostTask rt;
            rt.net = td.net;
            rt.path_scale = tasks[ti].path_scale;
            for (int k = 0; k < td.arc_cnt; k++) {
                const auto &r = results.at(td.arc_off + k);
                if (r.status == gpuroute::ARC_PATH_FULL)
                    rt.path_scale = std::min(tasks[ti].path_scale * 8, 1 << 20);
                auto &ad = nd.arcs.at(arc_map[ti][k].first).at(arc_map[ti][k].second);
                if (ctx->debug)
                    log("    TRACE %s arc %d.%d status %d cost %.4f expanded %d steps %d len %d attach %d\n",
                        ctx->nameOf(nets_by_udata.at(td.net)), arc_map[ti][k].first, arc_map[ti][k].second, r.status,
                        r.cost, r.expanded, r.steps, r.path_len,
                        r.path_len > 0 ? paths[r.path_off + r.path_len - 1].parent : -1);
                if (r.status == gpuroute::ARC_OK) {
                    apply_path(nd, ad, paths.data() + r.path_off, r.path_len);
                } else {
                    last_fail_status = r.status;
                    last_fail_reason = r.reason;
                    last_fail_expanded = r.expanded;
                    if (ctx->debug)
                        log_info("    arc %d.%d of net '%s' failed in the %s lane%s: status %d reason %d "
                                 "(expanded %d in %d steps)\n",
                                 arc_map[ti][k].first, arc_map[ti][k].second, ctx->nameOf(nets_by_udata.at(td.net)),
                                 large_lane ? "large" : "small", use_bb ? "" : " without bounding box", r.status,
                                 r.reason, r.expanded, r.steps);
                    rt.arcs.push_back(arc_map[ti][k]);
                }
            }
            if (!rt.arcs.empty())
                retry.push_back(std::move(rt));
        }
        return retry;
    }

    void route_batch(const std::vector<HostTask> &tasks)
    {
        // Small lane with bounding boxes first; then the large lane, still
        // bounded, for searches that outgrew the small scratch; and finally
        // the large lane without a bounding box, as router2 does for arcs
        // that find no path inside their box.
        std::vector<HostTask> retry = route_tasks(tasks, true, false);
        flush_state();
        if (retry.empty())
            return;
        if (ctx->verbose)
            log_info("    %zu nets retried in the large lane\n", retry.size());
        retry = route_tasks(retry, true, true);
        flush_state();
        if (retry.empty())
            return;
        if (ctx->verbose)
            log_info("    %zu nets retried without bounding box\n", retry.size());
        std::vector<HostTask> failed = route_tasks(retry, false, true);
        flush_state();
        for (auto &t : failed) {
            NetInfo *ni = nets_by_udata.at(t.net);
            auto &nd = nets.at(t.net);
            auto &a = t.arcs.front();
            auto &ad = nd.arcs.at(a.first).at(a.second);
            log_error("Failed to route arc %d.%d of net '%s', from %s to %s (status %d, reason %d, %d wires "
                      "expanded).\n",
                      a.first, a.second, ctx->nameOf(ni), ctx->nameOfWire(idx_to_wire[nd.src]),
                      ctx->nameOfWire(idx_to_wire[ad.sink]), last_fail_status, last_fail_reason, last_fail_expanded);
        }
    }

    // Group tasks into batches whose bounding boxes are disjoint, so that
    // nets routed concurrently cannot compete for the same wires. Tasks that
    // fit nowhere go into a final batch that tolerates overlap.
    std::vector<std::vector<HostTask>> make_batches(std::vector<HostTask> &tasks)
    {
        const int K = std::max(1, cfg.max_batches);
        const int W = ctx->getGridDimX() + 2, H = ctx->getGridDimY() + 2;
        std::vector<std::vector<uint8_t>> used(std::max(0, K - 1), std::vector<uint8_t>(size_t(W) * H, 0));
        std::vector<std::vector<HostTask>> batches(K);
        auto clamp_bb = [&](const BoundingBox &bb, int &x0, int &y0, int &x1, int &y1) {
            x0 = std::max(bb.x0, 0);
            y0 = std::max(bb.y0, 0);
            x1 = std::min(bb.x1, W - 1);
            y1 = std::min(bb.y1, H - 1);
        };
        for (auto &t : tasks) {
            int x0, y0, x1, y1;
            clamp_bb(nets.at(t.net).bb, x0, y0, x1, y1);
            bool placed = false;
            for (int b = 0; b < K - 1 && !placed; b++) {
                auto &grid = used[b];
                bool clash = false;
                for (int y = y0; y <= y1 && !clash; y++)
                    for (int x = x0; x <= x1; x++)
                        if (grid[size_t(y) * W + x]) {
                            clash = true;
                            break;
                        }
                if (clash)
                    continue;
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++)
                        grid[size_t(y) * W + x] = 1;
                batches[b].push_back(std::move(t));
                placed = true;
            }
            if (!placed)
                batches[K - 1].push_back(std::move(t));
        }
        std::vector<std::vector<HostTask>> out;
        for (auto &b : batches)
            if (!b.empty())
                out.push_back(std::move(b));
        return out;
    }

    void update_congestion()
    {
        total_wire_use = 0;
        overused_wires = 0;
        total_overuse = 0;
        failed_nets.clear();
        for (size_t i = 0; i < nets.size(); i++) {
            auto &nd = nets.at(i);
            for (auto &w : nd.wires) {
                ++total_wire_use;
                if (occ[w.first] > 1)
                    failed_nets.insert(int(i));
            }
        }
        for (size_t w = 0; w < occ.size(); w++) {
            if (occ[w] > 1) {
                ++overused_wires;
                total_overuse += occ[w] - 1;
                if (curr_cong_weight > 0) {
                    hist[w] = std::min(1e9f, hist[w] + (occ[w] - 1) * hist_cong_weight);
                    mark_dirty(int32_t(w));
                }
            }
        }
        for (int n : failed_nets) {
            auto &nd = nets.at(n);
            ++nd.fail_count;
            if ((nd.fail_count % 3) == 0)
                ctx->expandBoundingBox(nd.bb);
        }
    }

    // ------------------------------------------------------------------
    // Final binding into the Arch (ported from router2)

    bool bind_and_check(NetInfo *net, store_index<PortRef> usr_idx, int phys_pin)
    {
        bool success = true;
        auto &nd = nets.at(net->udata);
        auto &ad = nd.arcs.at(usr_idx.idx()).at(phys_pin);
        auto &usr = net->users.at(usr_idx);
        WireId src = ctx->getNetinfoSourceWire(net);
        if (src == WireId())
            return true;
        WireId dst = ctx->getNetinfoSinkWire(net, usr, phys_pin);
        if (dst == WireId())
            return true;
        if (!ad.routed) {
            if ((src == dst) && ctx->getBoundWireNet(dst) != net)
                ctx->bindWire(src, net, STRENGTH_WEAK);
            return true;
        }
        int32_t cursor = ad.sink;
        std::vector<PipId> to_bind;
        while (cursor != nd.src) {
            WireId cw = idx_to_wire[cursor];
            if (!ctx->checkWireAvail(cw)) {
                NetInfo *bound_net = ctx->getBoundWireNet(cw);
                if (bound_net != net) {
                    if (ctx->verbose)
                        log_info("Failed to bind wire %s to net %s, bound to net %s\n", ctx->nameOfWire(cw),
                                 net->name.c_str(ctx), bound_net ? bound_net->name.c_str(ctx) : "nullptr");
                    success = false;
                    break;
                }
            }
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end())
                log_error("Internal error; incomplete route tree for arc %d of net %s.\n", usr_idx.idx(),
                          ctx->nameOf(net));
            PipId p = fnd->second.pip;
            if (p == PipId())
                log_error("Internal error; route tree of net %s has no pip driving %s.\n", ctx->nameOf(net),
                          ctx->nameOfWire(cw));
            if (ctx->checkPipAvailForNet(p, net)) {
                if (ctx->getBoundPipNet(p) == nullptr)
                    to_bind.push_back(p);
            } else if (!ad.pre_routed || ctx->getBoundPipNet(p) != net) {
                if (ctx->verbose)
                    log_info("Failed to bind pip %s to net %s\n", ctx->nameOfPip(p), net->name.c_str(ctx));
                success = false;
                break;
            }
            cursor = fnd->second.parent;
        }
        if (success) {
            if (ctx->getBoundWireNet(src) == nullptr)
                ctx->bindWire(src, net, STRENGTH_WEAK);
            for (auto tb : to_bind)
                ctx->bindPip(tb, net, STRENGTH_WEAK);
        } else {
            ripup_arc(nd, ad);
            failed_nets.insert(net->udata);
        }
        return success;
    }

    bool bind_and_check_all()
    {
        ctx->check();
        bool success = true;
        std::vector<WireId> net_wires;
        for (auto net : nets_by_udata) {
            net_wires.clear();
            for (auto &w : net->wires)
                if (w.second.strength <= STRENGTH_STRONG)
                    net_wires.push_back(w.first);
            for (auto w : net_wires)
                ctx->unbindWire(w);
            for (auto usr : net->users.enumerate()) {
                auto &nd = nets.at(net->udata);
                for (size_t phys_pin = 0; phys_pin < nd.arcs.at(usr.index.idx()).size(); phys_pin++) {
                    if (!bind_and_check(net, usr.index, int(phys_pin))) {
                        ++arch_fail;
                        success = false;
                    }
                }
            }
        }
        ctx->check();
        flush_state();
        return success;
    }

    int iter = 1;

    // Negotiated-congestion loop over the nets in route_queue until no wire
    // is overused (and, with --tmg-ripup, no arc fails slack)
    void negotiate()
    {
        do {
            auto istart = Clock::now();
            gpu_time = 0.0;
            ctx->sorted_shuffle(route_queue);
            if (timing_driven && int(route_queue.size()) >= 30) {
                for (auto n : route_queue) {
                    NetInfo *ni = nets_by_udata.at(n);
                    auto &nd = nets.at(n);
                    nd.max_crit = 0;
                    for (auto &usr : ni->users)
                        nd.max_crit = std::max(nd.max_crit, tmg.get_criticality(CellPortKey(usr)));
                }
                std::stable_sort(route_queue.begin(), route_queue.end(),
                                 [&](int a, int b) { return nets.at(a).max_crit > nets.at(b).max_crit; });
            }

            // Rip up every arc that is unrouted, congested or (optionally)
            // failing timing, and collect the work per net
            std::vector<HostTask> tasks;
            for (int n : route_queue) {
                NetInfo *ni = nets_by_udata.at(n);
                auto &nd = nets.at(n);
                if (ni->driver.cell == nullptr || nd.src < 0)
                    continue;
                bool failed_slack = false;
                for (auto usr : ni->users.enumerate())
                    failed_slack |= arc_failed_slack(ni, usr.index);
                HostTask t;
                t.net = n;
                for (auto usr : ni->users.enumerate()) {
                    auto &arcs = nd.arcs.at(usr.index.idx());
                    for (size_t j = 0; j < arcs.size(); j++) {
                        if (arcs[j].frozen)
                            continue;
                        if (!failed_slack && check_arc_routing(nd, arcs[j]))
                            continue;
                        ripup_arc(nd, arcs[j]);
                        t.arcs.emplace_back(usr.index.idx(), int(j));
                    }
                }
                if (t.arcs.empty())
                    continue;
                std::stable_sort(t.arcs.begin(), t.arcs.end(), [&](const std::pair<int, int> &a,
                                                                   const std::pair<int, int> &b) {
                    return arc_crit(ni, store_index<PortRef>(a.first)) > arc_crit(ni, store_index<PortRef>(b.first));
                });
                tasks.push_back(std::move(t));
            }
            flush_state();

            auto batches = make_batches(tasks);
            size_t ntasks = tasks.size();
            for (auto &b : batches)
                route_batch(b);

            update_route_delays(route_queue);
            route_queue.clear();
            update_congestion();
            flush_state();

            int tmgfail = 0;
            if (timing_driven)
                tmg.run(false);
            if (timing_driven && cfg.perf_profile) {
                float min_slack = std::numeric_limits<float>::max();
                int neg = 0;
                for (auto ni : nets_by_udata)
                    for (auto &usr : ni->users) {
                        float sl = tmg.get_setup_slack(CellPortKey(usr));
                        min_slack = std::min(min_slack, sl);
                        if (sl < 0)
                            neg++;
                    }
                log_info("        min setup slack %.3f ns, %d arcs with negative slack\n",
                         ctx->getDelayNS(delay_t(min_slack)), neg);
            }
            if (timing_driven_ripup && iter < 1500) {
                for (size_t i = 0; i < nets_by_udata.size(); i++) {
                    NetInfo *ni = nets_by_udata.at(i);
                    for (auto usr : ni->users.enumerate()) {
                        if (arc_failed_slack(ni, usr.index)) {
                            failed_nets.insert(int(i));
                            ++tmgfail;
                        }
                    }
                }
                // Stop ripping up for timing once it has stopped helping;
                // the remaining arcs are already on their best routes. Only
                // count iterations in which arcs actually failed slack.
                if (tmgfail < best_tmgfail) {
                    best_tmgfail = tmgfail;
                    tmgfail_stall = 0;
                } else if (tmgfail > 0 && ++tmgfail_stall >= cfg.tmg_ripup_patience) {
                    log_info("    timing-driven rip-up made no progress for %d iterations; %d arcs still fail slack\n",
                             cfg.tmg_ripup_patience, tmgfail);
                    timing_driven_ripup = false;
                    if (overused_wires == 0) {
                        failed_nets.clear();
                        tmgfail = 0;
                    }
                }
            }
            for (auto cn : failed_nets)
                route_queue.push_back(cn);

            log_info("    iter=%d wires=%d overused=%d overuse=%d tmgfail=%d nets=%zu batches=%zu archfail=%s\n", iter,
                     total_wire_use, overused_wires, total_overuse, tmgfail, ntasks, batches.size(),
                     (overused_wires > 0 || tmgfail > 0) ? "NA" : std::to_string(arch_fail).c_str());
            if (cfg.perf_profile) {
                const auto &bs = backend->stats();
                log_info("        iteration %.3fs of which backend %.3fs; %lld arcs, %lld wires expanded\n",
                         secs_since(istart), gpu_time, (long long)(bs.arcs_routed - last_arcs),
                         (long long)(bs.wires_expanded - last_expanded));
                last_arcs = bs.arcs_routed;
                last_expanded = bs.wires_expanded;
            }
            ++iter;
            if (curr_cong_weight < 1e9)
                curr_cong_weight += cfg.curr_cong_mult;
            if (!failed_nets.empty() && (iter % 100) == 0) {
                int shown = 0;
                for (size_t w = 0; w < occ.size() && shown < 5; w++) {
                    if (occ[w] <= 1)
                        continue;
                    std::string users;
                    for (int n : failed_nets) {
                        auto &nd = nets.at(n);
                        auto fnd = nd.wires.find(int32_t(w));
                        if (fnd == nd.wires.end())
                            continue;
                        bool frozen = false;
                        for (auto &arcs : nd.arcs)
                            for (auto &ad : arcs)
                                frozen |= ad.frozen;
                        users += stringf(" %s(count=%d%s)", ctx->nameOf(nets_by_udata.at(n)), fnd->second.count,
                                         frozen ? ",frozen" : "");
                    }
                    log_info("        stalled: wire %s occ=%d reserved=%d used by%s\n", ctx->nameOfWire(idx_to_wire[w]),
                             occ[w], reserved[w], users.c_str());
                    shown++;
                }
            }
            if (iter > cfg.max_iter && !failed_nets.empty())
                log_error("GPU router did not converge after %d iterations (%d overused wires).\n", cfg.max_iter,
                          overused_wires);
        } while (!failed_nets.empty());
    }

    // Reserve every wire on the arc's path for its net (soft) and freeze it
    void freeze_arc(int net, ArcData &ad)
    {
        auto &nd = nets.at(net);
        int32_t cursor = ad.sink;
        while (true) {
            if (reserved[cursor] == -1) {
                reserved[cursor] = net | gpuroute::RESERVED_SOFT;
                mark_dirty(cursor);
            }
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end() || fnd->second.parent == -1 || cursor == nd.src)
                break;
            cursor = fnd->second.parent;
        }
        ad.frozen = true;
    }

    bool arc_uses_wire(const NetData &nd, const ArcData &ad, int32_t wire) const
    {
        int32_t cursor = ad.sink;
        while (true) {
            if (cursor == wire)
                return true;
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end() || fnd->second.parent == -1 || cursor == nd.src)
                return false;
            cursor = fnd->second.parent;
        }
    }

    // Drop and rebuild the soft reservations of a net from its frozen arcs
    void rebuild_soft_reservations(int net)
    {
        auto &nd = nets.at(net);
        for (auto &w : nd.wires)
            if (reserved[w.first] == (net | gpuroute::RESERVED_SOFT)) {
                reserved[w.first] = -1;
                mark_dirty(w.first);
            }
        for (auto &arcs : nd.arcs)
            for (auto &ad : arcs)
                if (ad.frozen)
                    freeze_arc(net, ad);
    }

    // A repaired arc of `net` was routed through other nets' soft
    // reservations. Unfreeze the frozen arcs holding them when they have at
    // least as much slack as the repaired arc; otherwise give the route up.
    void displace_for(int net, std::pair<int, int> arc)
    {
        NetInfo *ni = nets_by_udata.at(net);
        auto &nd = nets.at(net);
        auto &ad = nd.arcs.at(arc.first).at(arc.second);
        float my_slack = tmg.get_setup_slack(CellPortKey(ni->users.at(store_index<PortRef>(arc.first))));
        std::vector<std::pair<int, std::pair<int, int>>> victims; // (net, (user, phys))
        bool ok = true;
        int32_t cursor = ad.sink;
        while (ok) {
            int32_t r = reserved[cursor];
            int owner = gpuroute::reserved_owner(r);
            if (owner != -1 && owner != net) {
                if (!(r & gpuroute::RESERVED_SOFT)) {
                    ok = false; // hard reservation: should not happen
                    break;
                }
                NetInfo *oi = nets_by_udata.at(owner);
                auto &od = nets.at(owner);
                for (auto usr : oi->users.enumerate()) {
                    auto &arcs = od.arcs.at(usr.index.idx());
                    for (size_t j = 0; j < arcs.size(); j++) {
                        if (!arcs[j].frozen || !arc_uses_wire(od, arcs[j], cursor))
                            continue;
                        float their_slack = tmg.get_setup_slack(CellPortKey(usr.value));
                        if (their_slack < my_slack + cfg.repair_displace_margin) {
                            ok = false;
                            break;
                        }
                        victims.emplace_back(owner, std::make_pair(usr.index.idx(), int(j)));
                    }
                    if (!ok)
                        break;
                }
            }
            auto fnd = nd.wires.find(cursor);
            if (fnd == nd.wires.end() || fnd->second.parent == -1 || cursor == nd.src)
                break;
            cursor = fnd->second.parent;
        }
        if (!ok) {
            ripup_arc(nd, ad); // leave it for the normal negotiation
            return;
        }
        pool<int> touched;
        for (auto &v : victims) {
            nets.at(v.first).arcs.at(v.second.first).at(v.second.second).frozen = false;
            touched.insert(v.first);
        }
        for (int o : touched)
            rebuild_soft_reservations(o);
        displaced_total += int(victims.size());
    }
    int displaced_total = 0;

    // Arcs that still fail slack after negotiation are re-routed at pure
    // delay with their tree costed by upstream delay, one net at a time so
    // repaired arcs never compete, and their wires are reserved so the
    // negotiation loop moves the displaced non-critical arcs instead.
    void timing_repair()
    {
        int repaired_total = 0, improve_rounds = 0;
        float best_wns = std::numeric_limits<float>::lowest();
        // Snapshot of the best state seen, restored if a later round made
        // the worst slack worse
        struct Snapshot
        {
            std::vector<NetData> nets;
            std::vector<int32_t> occ, reserved;
            std::vector<float> hist;
            float wns = std::numeric_limits<float>::lowest();
        } best;
        auto take_snapshot = [&](float wns) {
            best.nets = nets;
            best.occ = occ;
            best.reserved = reserved;
            best.hist = hist;
            best.wns = wns;
        };
        auto restore_snapshot = [&]() {
            nets = best.nets;
            occ = best.occ;
            reserved = best.reserved;
            hist = best.hist;
            for (size_t w = 0; w < occ.size(); w++)
                mark_dirty(int32_t(w));
            flush_state();
            std::vector<int> all;
            for (size_t i = 0; i < nets.size(); i++)
                all.push_back(int(i));
            update_route_delays(all);
            tmg.run(false);
        };
        for (int round = 1; round <= cfg.repair_rounds; round++) {
            tmg.run(false);
            // Worst slack over the arcs that can still be repaired
            float wns = std::numeric_limits<float>::max();
            for (size_t i = 0; i < nets_by_udata.size(); i++) {
                NetInfo *ni = nets_by_udata.at(i);
                auto &nd = nets.at(i);
                if (ni->driver.cell == nullptr || nd.src < 0)
                    continue;
                for (auto usr : ni->users.enumerate()) {
                    bool repairable = false;
                    for (auto &ad : nd.arcs.at(usr.index.idx()))
                        repairable |= (!ad.frozen && !ad.pre_routed);
                    float slack = tmg.get_setup_slack(CellPortKey(usr.value));
                    if (repairable && slack != std::numeric_limits<float>::lowest())
                        wns = std::min(wns, slack);
                }
            }
            if (wns == std::numeric_limits<float>::max())
                break;
            if (round > 1 && wns <= best_wns + 1.0f) {
                log_info("    timing repair round %d: worst slack %.3f ns did not improve on %.3f ns; stopping\n",
                         round, ctx->getDelayNS(delay_t(wns)), ctx->getDelayNS(delay_t(best_wns)));
                if (wns < best.wns) {
                    log_info("    restoring the routing of the best round\n");
                    restore_snapshot();
                }
                break;
            }
            best_wns = std::max(best_wns, wns);
            take_snapshot(wns);
            // Repair everything failing, and while nothing fails, the arcs
            // within repair_band of the worst slack (Fmax improvement) for a
            // bounded number of rounds
            float thresh = std::max(cfg.repair_slack, wns + cfg.repair_band);
            if (wns >= cfg.repair_slack) {
                if (cfg.repair_band <= 0.0f || ++improve_rounds > cfg.repair_improve_rounds) {
                    log_info("    timing repair round %d: no arcs fail slack (worst %.3f ns)\n", round,
                             ctx->getDelayNS(delay_t(wns)));
                    break;
                }
            }
            // (slack, net, (user, phys))
            std::vector<std::tuple<float, int, std::pair<int, int>>> failing;
            for (size_t i = 0; i < nets_by_udata.size(); i++) {
                NetInfo *ni = nets_by_udata.at(i);
                auto &nd = nets.at(i);
                if (ni->driver.cell == nullptr || nd.src < 0)
                    continue;
                for (auto usr : ni->users.enumerate()) {
                    float slack = tmg.get_setup_slack(CellPortKey(usr.value));
                    if (slack >= thresh || slack == std::numeric_limits<float>::lowest())
                        continue;
                    auto &arcs = nd.arcs.at(usr.index.idx());
                    for (size_t j = 0; j < arcs.size(); j++)
                        if (!arcs[j].frozen && !arcs[j].pre_routed)
                            failing.emplace_back(slack, int(i), std::make_pair(usr.index.idx(), int(j)));
                }
            }
            if (failing.empty()) {
                log_info("    timing repair round %d: nothing left to repair\n", round);
                break;
            }
            std::stable_sort(failing.begin(), failing.end(),
                             [](const auto &a, const auto &b) { return std::get<0>(a) < std::get<0>(b); });
            // group by net, keeping the worst-slack-first order of nets
            std::vector<HostTask> tasks;
            dict<int, size_t> task_of;
            for (auto &f : failing) {
                int net = std::get<1>(f);
                auto fnd = task_of.find(net);
                if (fnd == task_of.end()) {
                    task_of[net] = tasks.size();
                    HostTask t;
                    t.net = net;
                    tasks.push_back(t);
                    fnd = task_of.find(net);
                }
                tasks[fnd->second].arcs.push_back(std::get<2>(f));
            }
            int repaired = 0, failed_repairs = 0;
            repair_mode = true;
            for (auto &t : tasks) {
                auto &nd = nets.at(t.net);
                for (auto &a : t.arcs)
                    ripup_arc(nd, nd.arcs.at(a.first).at(a.second));
                flush_state();
                std::vector<HostTask> one{t};
                std::vector<HostTask> retry = route_tasks(one, true, false);
                if (!retry.empty())
                    retry = route_tasks(retry, false, true);
                if (!retry.empty() && cfg.repair_displace) {
                    // The minimum-delay route needs wires frozen by other
                    // arcs. Take them if those arcs have more slack than
                    // this one, and unfreeze them so negotiation moves them.
                    ignore_soft = true;
                    std::vector<HostTask> still = route_tasks(retry, false, true);
                    ignore_soft = false;
                    pool<std::pair<int, int>> still_unrouted;
                    for (auto &r : still)
                        for (auto &a : r.arcs)
                            still_unrouted.insert(a);
                    for (auto &r : retry)
                        for (auto &a : r.arcs)
                            if (!still_unrouted.count(a))
                                displace_for(t.net, a);
                    retry = still;
                }
                pool<std::pair<int, int>> unrouted;
                for (auto &r : retry)
                    for (auto &a : r.arcs)
                        unrouted.insert(a);
                for (auto &a : t.arcs) {
                    auto &ad = nd.arcs.at(a.first).at(a.second);
                    if (unrouted.count(a) || !ad.routed) {
                        failed_repairs++;
                        continue; // left unrouted; negotiate() routes it normally
                    }
                    freeze_arc(t.net, ad);
                    repaired++;
                }
                flush_state();
            }
            repair_mode = false;
            repaired_total += repaired;
            log_info("    timing repair round %d: worst slack %.3f ns, %zu arcs below %.3f ns, %d re-routed at pure "
                     "delay, %d not routable\n",
                     round, ctx->getDelayNS(delay_t(wns)), failing.size(), ctx->getDelayNS(delay_t(thresh)), repaired,
                     failed_repairs);
            // Re-negotiate: the repaired nets (for delay bookkeeping) and
            // every net now sharing a reserved wire
            update_congestion();
            for (auto &t : tasks)
                route_queue.push_back(t.net);
            for (auto cn : failed_nets)
                route_queue.push_back(cn);
            std::sort(route_queue.begin(), route_queue.end());
            route_queue.erase(std::unique(route_queue.begin(), route_queue.end()), route_queue.end());
            negotiate();
        }
        if (displaced_total > 0)
            log_info("    timing repair displaced %d frozen arcs with more slack\n", displaced_total);
        if (repaired_total > 0) {
            tmg.run(false);
            float wns = std::numeric_limits<float>::max();
            for (auto ni : nets_by_udata)
                for (auto &usr : ni->users) {
                    float sl = tmg.get_setup_slack(CellPortKey(usr));
                    if (sl != std::numeric_limits<float>::lowest())
                        wns = std::min(wns, sl);
                }
            if (best.wns != std::numeric_limits<float>::lowest() && wns < best.wns - 1.0f) {
                log_info("    final worst slack %.3f ns is below the best round's %.3f ns; restoring it\n",
                         ctx->getDelayNS(delay_t(wns)), ctx->getDelayNS(delay_t(best.wns)));
                restore_snapshot();
            }
            log_info("    timing repair froze %d arcs\n", repaired_total);
        }
    }

    // ------------------------------------------------------------------

    bool operator()()
    {
        auto rstart = Clock::now();
        log_info("Running the GPU router...\n");

        std::string err;
        if (cfg.cpu_backend) {
            backend = gpuroute::create_cpu_backend();
        } else {
            backend = gpuroute::create_device_backend(cfg.device, err);
            if (!backend) {
                log_warning("GPU router: %s; falling back to the CPU reference backend.\n", err.c_str());
                backend = gpuroute::create_cpu_backend();
            }
        }
        if (!cfg.cpu_backend && gpuroute::device_backend_available() && ctx->verbose)
            log_info("    GPU devices: %s\n", gpuroute::describe_devices().c_str());
        if (cfg.cpu_lane_nets > 0 && std::string(backend->name()) != "cpu-reference")
            cpu_backend = gpuroute::create_cpu_backend();

        log_info("Setting up routing resources...\n");
        setup_net_indices();
        setup_graph();
        setup_nets();
        find_all_reserved_wires();
        fit_estimate();

        gpuroute::GraphData gd;
        gd.n_wires = int32_t(idx_to_wire.size());
        gd.n_edges = int64_t(out_dst.size());
        gd.out_off = out_off.data();
        gd.out_dst = out_dst.data();
        gd.edge_cost = edge_cost.data();
        gd.wire_x = wx.data();
        gd.wire_y = wy.data();
        gd.wire_reserved = reserved.data();
        gd.wire_flags = wflags.data();
        gd.wire_occ = occ.data();
        gd.wire_hist = hist.data();
        auto tup = Clock::now();
        if (!backend->init(gd, cfg.small_slots, cfg.small_bits, cfg.large_slots, cfg.large_bits, err))
            log_error("GPU router backend initialisation failed: %s\n", err.c_str());
        if (cpu_backend &&
            !cpu_backend->init(gd, cfg.small_slots, cfg.small_bits, cfg.large_slots, cfg.large_bits, err))
            log_error("GPU router CPU lane initialisation failed: %s\n", err.c_str());
        log_info("    backend %s ready in %.2fs (estimate %.3f ns/x, %.3f ns/y)\n", backend->name(), secs_since(tup),
                 est_x, est_y);
        // The backend snapshot already contains the setup-time state
        dirty_wires.clear();
        std::fill(dirty_mark.begin(), dirty_mark.end(), 0);

        curr_cong_weight = cfg.init_curr_cong_weight;
        hist_cong_weight = cfg.hist_cong_weight;
        timing_driven = ctx->setting<bool>("timing_driven");
        if (ctx->settings.count(ctx->id("router/tmg_ripup")))
            timing_driven_ripup = timing_driven && ctx->setting<bool>("router/tmg_ripup");

        std::unique_lock<Context> lock{*ctx};

        for (size_t i = 0; i < nets_by_udata.size(); i++)
            route_queue.push_back(int(i));

        log_info("Running main router loop...\n");
        if (timing_driven)
            tmg.run(true);
        negotiate();
        if (timing_driven && cfg.repair_rounds > 0)
            timing_repair();
        // Bind into the Arch; anything the Arch rejects is negotiated again
        while (true) {
            bind_and_check_all();
            if (failed_nets.empty())
                break;
            log_info("    %zu nets rejected by the architecture, re-routing\n", failed_nets.size());
            for (auto cn : failed_nets)
                route_queue.push_back(cn);
            negotiate();
        }

        const auto &st = backend->stats();
        log_info("GPU router time %.02fs (backend %s: %lld launches, %lld arcs, %lld wires expanded, %.2fs routing, "
                 "%.2fs transfers)\n",
                 secs_since(rstart), backend->name(), (long long)st.launches, (long long)st.arcs_routed,
                 (long long)st.wires_expanded, st.route_seconds, st.transfer_seconds);
        if (cpu_backend) {
            const auto &cs = cpu_backend->stats();
            log_info("    CPU lane: %lld batches, %lld arcs, %lld wires expanded, %.2fs\n", (long long)cs.launches,
                     (long long)cs.arcs_routed, (long long)cs.wires_expanded, cs.route_seconds);
        }

        log_info("Running router1 to check that route is legal...\n");
        lock.unlock();
        return router1(ctx, Router1Cfg(ctx));
    }
};

} // namespace

bool gpurouter(Context *ctx, const GpuRouterCfg &cfg)
{
    GpuRouter rt(ctx, cfg);
    return rt();
}

GpuRouterCfg::GpuRouterCfg(Context *ctx)
{
    bb_margin_x = ctx->setting<int>("gpurouter/bbMargin/x", 3);
    bb_margin_y = ctx->setting<int>("gpurouter/bbMargin/y", 3);
    init_curr_cong_weight = ctx->setting<float>("gpurouter/initCurrCongWeight", 0.5f);
    hist_cong_weight = ctx->setting<float>("gpurouter/histCongWeight", 1.0f);
    curr_cong_mult = ctx->setting<float>("gpurouter/currCongWeightMult", 2.0f);
    estimate_weight = ctx->setting<float>("gpurouter/estimateWeight", 1.25f);
    bias_cost_factor = ctx->setting<float>("gpurouter/biasCostFactor", 0.25f);
    seed_delay_weight = ctx->setting<float>("gpurouter/seedDelayWeight", 1.0f);
    seed_delay_floor = ctx->setting<float>("gpurouter/seedDelayFloor", 0.0f);
    crit_weight_floor = ctx->setting<float>("gpurouter/critWeightFloor", 0.05f);
    crit_weight_mode = ctx->setting<int>("gpurouter/critWeightMode", 0);
    repair_rounds = ctx->setting<int>("gpurouter/repairRounds", 10);
    repair_slack = ctx->setting<float>("gpurouter/repairSlack", 0.0f);
    repair_band = ctx->setting<float>("gpurouter/repairBand", 300.0f);
    repair_improve_rounds = ctx->setting<int>("gpurouter/repairImproveRounds", 4);
    repair_displace = ctx->setting<bool>("gpurouter/repairDisplace", true);
    repair_displace_margin = ctx->setting<float>("gpurouter/repairDisplaceMargin", 0.0f);
    cpu_lane_nets = ctx->setting<int>("gpurouter/cpuLaneNets", 0);
    crit_exponent = ctx->setting<float>("gpurouter/critExponent", 2.0f);
    load_penalty = ctx->setting<float>("gpurouter/loadPenalty", 0.0f);
    pip_adder = ctx->setting<float>("gpurouter/pipAdder", 0.0f);
    tmg_ripup_patience = ctx->setting<int>("gpurouter/tmgRipupPatience", 8);
    expand_k = ctx->setting<int>("gpurouter/expandK", 256);
    expand_div = ctx->setting<int>("gpurouter/expandDiv", 0);
    max_batches = ctx->setting<int>("gpurouter/maxBatches", 12);
    small_slots = ctx->setting<int>("gpurouter/smallSlots", 384);
    large_slots = ctx->setting<int>("gpurouter/largeSlots", 4);
    small_bits = ctx->setting<int>("gpurouter/smallBits", 16);
    large_bits = ctx->setting<int>("gpurouter/largeBits", 22);
    device = ctx->setting<int>("gpurouter/device", -1);
    cpu_backend = ctx->setting<bool>("gpurouter/cpu", false);
    perf_profile = ctx->setting<bool>("gpurouter/perfProfile", false);
    max_iter = ctx->setting<int>("gpurouter/maxIter", 2000);
}

NEXTPNR_NAMESPACE_END
