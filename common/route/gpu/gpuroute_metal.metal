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
 *  Metal Shading Language port of the GPU routing kernel in
 *  gpuroute_kernel.cuh, for Apple silicon. The algorithm, the cost
 *  expressions and their evaluation order, the tie-breaking and the
 *  determinism argument are those of the HIP/CUDA kernel; see the comment
 *  at the top of that file. A change to one kernel must be made to both.
 *
 *  The one structural difference: Metal's 64-bit atomic min does not return
 *  the previous value, so the HIP/CUDA "apply the candidates" phase B is
 *  split in two. B1 inserts each candidate wire and applies the minimum over
 *  (g, parent edge). B2, after a barrier, re-queues the candidate that won
 *  its wire if it lowered g below the step-start value. That queues exactly
 *  the live frontier entries the HIP/CUDA kernel queues (one per improved
 *  wire, with the final g), and none of the stale ones it may add when
 *  candidates for one wire land in decreasing order.
 *
 *  Wire state updates need no kernel: the buffers live in memory shared with
 *  the CPU, and the host writes them between launches.
 *
 *  gpuroute_metal.mm embeds this file and compiles it at run time. The
 *  structs below mirror gpuroute_backend.h and are checked by size there.
 */

#include <metal_atomic>
#include <metal_stdlib>

using namespace metal;

// No fused multiply-add: the host backend evaluates the same cost
// expressions and must reach bit-identical values
#pragma METAL fp contract(off)

constant constexpr int BLOCK = 256;
constant constexpr uint EMPTY_KEY = 0xFFFFFFFFu;
constant constexpr uint NONE_SLOT = 0xFFFFFFFFu; // "parent" marker of a seed wire
constant constexpr ulong EMPTY_VAL = ~0ul;
constant constexpr uint INF_BITS = 0x7f800000u;
constant constexpr int NBINS = 128;
constant constexpr int NPILES = 4;
// B1 tags a candidate whose g is below its wire's step-start g in the top
// bit of its parent edge (edge indices are below 2^31)
constant constexpr uint IMPROVES_G = 0x80000000u;

constant constexpr int WIRE_UNAVAILABLE = 0x01;
constant constexpr int RESERVED_SOFT = 0x40000000;

constant constexpr int ARC_OK = 0, ARC_NO_PATH = 1, ARC_OVERFLOW = 2, ARC_PATH_FULL = 3;
constant constexpr int FAIL_NONE = 0, FAIL_PROBE = 1, FAIL_LOAD = 2, FAIL_PILE = 3;

struct RouteParams
{
    float est_x, est_y, est_weight, curr_cong_weight, bias_factor;
    float seed_delay_weight, seed_delay_floor, load_penalty, pip_adder;
    int expand_k, expand_div, use_bb, ignore_soft, ignore_hist, max_probe, exact;
};

struct TaskDesc
{
    int net;
    int tree_off, tree_cnt;
    int arc_off, arc_cnt;
    int path_off, path_cap;
    int cx, cy, hpwl, fanout;
    short bb_x0, bb_y0, bb_x1, bb_y1;
    int allow_reserved;
    int stay_in_halo;
    int shell_off, shell_cnt;
};

struct ArcDesc
{
    int sink;
    float crit_weight;
    float crit;
};

struct ArcResult
{
    int status, path_off, path_len, expanded, steps, reason;
    float cost;
};

struct PathEntry
{
    int wire, parent, edge;
    float delay;
};

struct LaunchArgs
{
    int n_wires;
    int ntasks;
    int cap;
};

struct BlockState
{
    atomic_int near_n, far_n, far2_n, cand_n;
    atomic_int fail;
    atomic_int inserted;
    atomic_int expanded;
    int steps;
    int path_pos;
    int far_idx;
    int decision; // 0 continue, 1 stop
    int reached;  // an earlier arc of the task already routed through the sink
    float binw;
    uint thr_bits;
    uint prune_bits;
    atomic_uint minf_bits, maxf_bits;
};

#define RELAXED memory_order_relaxed
#define SYNC() threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device)

static inline int ld(threadgroup atomic_int &a) { return atomic_load_explicit(&a, RELAXED); }
static inline void st_(threadgroup atomic_int &a, int v) { atomic_store_explicit(&a, v, RELAXED); }
static inline int inc(threadgroup atomic_int &a) { return atomic_fetch_add_explicit(&a, 1, RELAXED); }

static inline uint f2u(float f) { return as_type<uint>(f); }
static inline float u2f(uint u) { return as_type<float>(u); }
static inline ulong pack(float g, uint lo) { return (ulong(f2u(g)) << 32) | ulong(lo); }
static inline float pack_g(ulong v) { return u2f(uint(v >> 32)); }
static inline uint pack_gbits(ulong v) { return uint(v >> 32); }
static inline uint pack_lo(ulong v) { return uint(v); }

static inline uint hash32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static inline uint hash_step(uint x)
{
    // second hash for double hashing; odd so every slot is visited
    x ^= x >> 13;
    x *= 0x5bd1e995U;
    x ^= x >> 15;
    return (x << 1) | 1u;
}

// Lookups never overlap inserts (those run in phases of their own, between
// barriers), so they use plain, cached loads
static inline int table_find(const device uint *keys, int mask, int max_probe, uint w)
{
    uint i = hash32(w) & uint(mask);
    const uint step = hash_step(w);
    for (int p = 0; p < max_probe; p++) {
        uint k = keys[i];
        if (k == w)
            return int(i);
        if (k == EMPTY_KEY)
            return -1;
        i = (i + step) & uint(mask);
    }
    return -1;
}

// Returns the slot for wire w, inserting it if needed; -1 if the probe
// limit is hit. inserted is set when this call created the entry.
static inline int table_insert(device atomic_uint *keys, int mask, int max_probe, uint w, thread bool &inserted)
{
    uint i = hash32(w) & uint(mask);
    const uint step = hash_step(w);
    for (int p = 0; p < max_probe; p++) {
        uint k = atomic_load_explicit(&keys[i], RELAXED);
        if (k == w) {
            inserted = false;
            return int(i);
        }
        if (k == EMPTY_KEY) {
            // Metal only has a weak compare-exchange: retry spurious failures
            uint old = EMPTY_KEY;
            while (!atomic_compare_exchange_weak_explicit(&keys[i], &old, w, RELAXED, RELAXED) && old == EMPTY_KEY)
                ;
            if (old == EMPTY_KEY) {
                inserted = true;
                return int(i);
            }
            if (old == w) {
                inserted = false;
                return int(i);
            }
        }
        i = (i + step) & uint(mask);
    }
    return -1;
}

static inline float h_est(constant RouteParams &p, int x, int y, int sx, int sy)
{
    int dx = x - sx, dy = y - sy;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    return p.est_weight * (p.est_x * float(dx) + p.est_y * float(dy));
}

// Source wire of CSR edge e: the largest wire whose row starts at or before e
static inline int edge_src(const device int *out_off, int n_wires, int e)
{
    int lo = 0, hi = n_wires - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (out_off[mid] <= e)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static inline int pack_shell_tile(int x, int y)
{
    return int(uint(ushort(short(x))) | (uint(ushort(short(y))) << 16));
}

static inline bool shell_tile_hit(const device int *tiles, int off, int cnt, int x, int y)
{
    if (cnt <= 0)
        return false;
    const int key = pack_shell_tile(x, y);
    for (int i = 0; i < cnt; i++)
        if (tiles[off + i] == key)
            return true;
    return false;
}

[[max_total_threads_per_threadgroup(256)]] kernel void
route_kernel(const device int *out_off [[buffer(0)]], const device int *out_dst [[buffer(1)]],
             const device float *edge_cost [[buffer(2)]], const device uchar *edge_region [[buffer(3)]],
             const device uchar *edge_outside_halo [[buffer(4)]], const device short *wx [[buffer(5)]],
             const device short *wy [[buffer(6)]], const device int *reserved [[buffer(7)]],
             const device uchar *flags [[buffer(8)]], const device int *occ [[buffer(9)]],
             const device float *whist [[buffer(10)]], constant RouteParams &p [[buffer(11)]],
             constant LaunchArgs &la [[buffer(12)]], const device TaskDesc *tasks [[buffer(13)]],
             const device ArcDesc *arcs [[buffer(14)]], const device int *seeds [[buffer(15)]],
             const device float *seed_delay [[buffer(16)]], const device float *seed_load [[buffer(17)]],
             const device int *shell_tiles [[buffer(18)]], device atomic_uint *sc_keys [[buffer(19)]],
             device ulong *sc_vals [[buffer(20)]], device float *sc_sdelay [[buffer(21)]],
             device float *sc_sload [[buffer(22)]], device ulong *sc_piles [[buffer(23)]],
             device uint *sc_cand_par [[buffer(24)]], device ulong *sc_best [[buffer(25)]],
             device ArcResult *results [[buffer(26)]], device PathEntry *paths [[buffer(27)]],
             uint tid_u [[thread_index_in_threadgroup]], uint bid [[threadgroup_position_in_grid]],
             uint nblocks [[threadgroups_per_grid]])
{
    threadgroup BlockState st;
    threadgroup atomic_int hist[NBINS];
    const int tid = int(tid_u);
    const int cap = la.cap;
    const int mask = cap - 1;
    device atomic_uint *keys = sc_keys + size_t(bid) * cap;
    const device uint *lookup = (const device uint *)keys;
    device ulong *vals = sc_vals + size_t(bid) * cap;
    device float *sdelay = sc_sdelay + size_t(bid) * cap;
    device float *sload = sc_sload + size_t(bid) * cap;
    device ulong *pilebase = sc_piles + size_t(bid) * NPILES * size_t(cap);
    device ulong *near = pilebase + 2 * size_t(cap);
    device ulong *cand = pilebase + 3 * size_t(cap);
    device uint *cand_par = sc_cand_par + size_t(bid) * cap;
    device ulong *best = sc_best + bid;

    for (int t = int(bid); t < la.ntasks; t += int(nblocks)) {
        const TaskDesc task = tasks[t];
        if (tid == 0)
            st.path_pos = 0;
        SYNC();

        for (int a = 0; a < task.arc_cnt; a++) {
            const ArcDesc arc = arcs[task.arc_off + a];
            const int sink = arc.sink;
            const int sx = wx[sink], sy = wy[sink];

            // 1. clear the table
            for (int i = tid; i < cap; i += BLOCK) {
                atomic_store_explicit(&keys[i], EMPTY_KEY, RELAXED);
                vals[i] = EMPTY_VAL;
            }
            if (tid == 0) {
                st_(st.near_n, 0);
                st_(st.far_n, 0);
                st_(st.far2_n, 0);
                st_(st.cand_n, 0);
                st_(st.fail, 0);
                st_(st.inserted, 0);
                st_(st.expanded, 0);
                st.steps = 0;
                st.far_idx = 0;
                st.decision = 0;
                st.reached = 0;
                atomic_store_explicit(&st.minf_bits, INF_BITS, RELAXED);
                atomic_store_explicit(&st.maxf_bits, 0u, RELAXED);
                st.prune_bits = INF_BITS;
                *best = EMPTY_VAL;
            }
            SYNC();

            // 2. seeds: the net tree plus the wires added by earlier arcs of this task
            {
                const int nseed = task.tree_cnt + st.path_pos;
                device ulong *far = pilebase + size_t(st.far_idx) * cap;
                const float seed_scale =
                        p.seed_delay_weight * (p.seed_delay_floor + (1.0f - p.seed_delay_floor) * arc.crit);
                for (int i = tid; i < nseed; i += BLOCK) {
                    int w;
                    float d, ldv = 0.0f;
                    if (i < task.tree_cnt) {
                        w = seeds[task.tree_off + i];
                        d = seed_delay[task.tree_off + i];
                        ldv = seed_load[task.tree_off + i];
                    } else {
                        const PathEntry pe = paths[task.path_off + (i - task.tree_cnt)];
                        w = pe.wire;
                        d = pe.delay;
                    }
                    // Not an attach point: this wire is the arc's destination.
                    // On the path of an earlier arc of the task it is already
                    // routed, and the arc shares that branch (see 4.).
                    if (w == sink) {
                        if (i >= task.tree_cnt)
                            st.reached = 1;
                        continue;
                    }
                    bool ins;
                    int s = table_insert(keys, mask, p.max_probe, uint(w), ins);
                    if (s < 0) {
                        st_(st.fail, FAIL_PROBE);
                        continue;
                    }
                    if (!ins)
                        continue;
                    inc(st.inserted);
                    if (d < 0.0f) {
                        // blocked tree wire: present so nothing relaxes into it, never expanded
                        vals[s] = pack(u2f(INF_BITS), NONE_SLOT);
                        sdelay[s] = 0.0f;
                        sload[s] = 0.0f;
                        continue;
                    }
                    const float g0 = seed_scale * d;
                    vals[s] = pack(g0, NONE_SLOT);
                    sdelay[s] = d;
                    sload[s] = ldv;
                    int pos = inc(st.far_n);
                    if (pos < cap)
                        far[pos] = pack(g0, uint(w));
                    else
                        st_(st.fail, FAIL_PILE);
                }
            }
            SYNC();
            if (tid == 0) {
                if (ld(st.far_n) > cap)
                    st_(st.far_n, cap);
                if (ld(st.fail) || st.reached)
                    st.decision = 1;
            }
            SYNC();

            // 3. main loop: each step expands the ~expand_k frontier entries
            //    with the lowest f = g + h, all children go back to the frontier
            while (st.decision == 0) {
                device ulong *far = pilebase + size_t(st.far_idx) * cap;
                device ulong *far2 = pilebase + size_t(st.far_idx ^ 1) * cap;
                const int fn = ld(st.far_n);
                const float prune = u2f(st.prune_bits);

                // pass 1: drop stale and pruned entries, track the f range
                for (int i = tid; i < fn; i += BLOCK) {
                    ulong e = far[i];
                    const uint v = pack_lo(e);
                    const uint gbits = pack_gbits(e);
                    int sv = table_find(lookup, mask, p.max_probe, v);
                    if (sv < 0 || pack_gbits(vals[sv]) != gbits)
                        continue;
                    const float f = u2f(gbits) + h_est(p, wx[v], wy[v], sx, sy);
                    if (f >= prune)
                        continue;
                    atomic_fetch_min_explicit(&st.minf_bits, f2u(f), RELAXED);
                    atomic_fetch_max_explicit(&st.maxf_bits, f2u(f), RELAXED);
                    int pos = inc(st.far2_n);
                    if (pos < cap)
                        far2[pos] = e;
                    else
                        st_(st.fail, FAIL_PILE);
                }
                SYNC();
                if (tid == 0) {
                    const ulong bv = *best;
                    const bool have_best = bv != EMPTY_VAL;
                    const float bestg = have_best ? pack_g(bv) : u2f(INF_BITS);
                    const uint minf_bits = atomic_load_explicit(&st.minf_bits, RELAXED);
                    const float minf = u2f(minf_bits);
                    if (ld(st.fail) || ld(st.far2_n) == 0 || minf_bits == INF_BITS || (have_best && minf >= bestg)) {
                        st.decision = 1;
                    } else {
                        const float range = u2f(atomic_load_explicit(&st.maxf_bits, RELAXED)) - minf;
                        st.binw = range > 1e-6f ? range / float(NBINS) : 0.0f;
                        if (ld(st.far2_n) > cap)
                            st_(st.far2_n, cap);
                    }
                }
                for (int i = tid; i < NBINS; i += BLOCK)
                    atomic_store_explicit(&hist[i], 0, RELAXED);
                SYNC();
                if (st.decision != 0)
                    break;

                // pass 2: histogram of f over the compacted frontier
                const int fn2 = ld(st.far2_n);
                const float minf = u2f(atomic_load_explicit(&st.minf_bits, RELAXED));
                const float binw = st.binw;
                if (binw > 0.0f) {
                    for (int i = tid; i < fn2; i += BLOCK) {
                        ulong e = far2[i];
                        const uint v = pack_lo(e);
                        const float f = pack_g(e) + h_est(p, wx[v], wy[v], sx, sy);
                        int b = int((f - minf) / binw);
                        b = b < 0 ? 0 : (b >= NBINS ? NBINS - 1 : b);
                        atomic_fetch_add_explicit(&hist[b], 1, RELAXED);
                    }
                }
                SYNC();
                if (tid == 0) {
                    float thr = u2f(INF_BITS);
                    if (binw > 0.0f) {
                        int cum = 0;
                        int want = p.expand_k;
                        if (p.expand_div > 0 && fn2 / p.expand_div > want)
                            want = fn2 / p.expand_div;
                        for (int b = 0; b < NBINS; b++) {
                            cum += atomic_load_explicit(&hist[b], RELAXED);
                            if (cum >= want) {
                                thr = minf + float(b + 1) * binw;
                                break;
                            }
                        }
                    }
                    st.thr_bits = f2u(thr);
                    st_(st.near_n, 0);
                    st_(st.far_n, 0);
                }
                SYNC();

                // pass 3: split the frontier into this step's expansion set and the rest
                {
                    const float thr = u2f(st.thr_bits);
                    for (int i = tid; i < fn2; i += BLOCK) {
                        ulong e = far2[i];
                        const uint v = pack_lo(e);
                        const float f = pack_g(e) + h_est(p, wx[v], wy[v], sx, sy);
                        // The bin boundary can round back to minf when the range
                        // is small relative to the costs. Always expand the minima
                        // so a nonempty frontier cannot repeat without progress.
                        if (f < thr || f == minf) {
                            int pos = inc(st.near_n);
                            if (pos < cap)
                                near[pos] = e;
                            else
                                st_(st.fail, FAIL_PILE);
                        } else {
                            int pos = inc(st.far_n);
                            if (pos < cap)
                                far[pos] = e;
                            else
                                st_(st.fail, FAIL_PILE);
                        }
                    }
                }
                SYNC();
                if (tid == 0) {
                    if (ld(st.near_n) > cap)
                        st_(st.near_n, cap);
                    if (ld(st.far_n) > cap)
                        st_(st.far_n, cap);
                    st_(st.far2_n, 0);
                    atomic_store_explicit(&st.minf_bits, INF_BITS, RELAXED);
                    atomic_store_explicit(&st.maxf_bits, 0u, RELAXED);
                    const ulong bv = *best;
                    st.prune_bits = (bv != EMPTY_VAL) ? pack_gbits(bv) : INF_BITS;
                    if (ld(st.fail))
                        st.decision = 1;
                }
                SYNC();
                if (st.decision != 0)
                    break;

                // expansion pass, phase A: relax every edge of the expansion
                // set against the table state at the start of the step and
                // record candidates; nothing is written to the table yet
                {
                    const int nn = ld(st.near_n);
                    const float prune2 = u2f(st.prune_bits);
                    for (int i = tid; i < nn; i += BLOCK) {
                        ulong e = near[i];
                        const uint u = pack_lo(e);
                        const float gu = pack_g(e);
                        inc(st.expanded);
                        // branching off a tree wire that already drives other
                        // branches loads it further; charge for that
                        float branch_extra = 0.0f;
                        if (p.load_penalty > 0.0f) {
                            const int su = table_find(lookup, mask, p.max_probe, u);
                            if (su >= 0 && pack_lo(vals[su]) == NONE_SLOT)
                                branch_extra = p.load_penalty * sload[su];
                        }
                        const int e0 = out_off[u], e1 = out_off[u + 1];
                        for (int ei = e0; ei < e1; ei++) {
                            const int v = out_dst[ei];
                            float c = edge_cost[ei];
                            if (c < 0.0f || (edge_region[ei] && !task.allow_reserved))
                                continue;
                            c += p.pip_adder;
                            const int vx = wx[v], vy = wy[v];
                            if (task.stay_in_halo && edge_outside_halo[ei] &&
                                !shell_tile_hit(shell_tiles, task.shell_off, task.shell_cnt, vx, vy))
                                continue;
                            if (p.use_bb && (vx < task.bb_x0 || vx > task.bb_x1 || vy < task.bb_y0 || vy > task.bb_y1))
                                continue;
                            if (flags[v] & WIRE_UNAVAILABLE)
                                continue;
                            const int rsv = reserved[v];
                            if (rsv != -1 && (rsv & ~RESERVED_SOFT) != task.net &&
                                !(p.ignore_soft && (rsv & RESERVED_SOFT)))
                                continue;
                            const float hist_c = p.ignore_hist ? 1.0f : 1.0f + arc.crit_weight * (whist[v] - 1.0f);
                            const float pres = 1.0f + float(occ[v]) * p.curr_cong_weight * arc.crit_weight;
                            int bdx = vx - task.cx, bdy = vy - task.cy;
                            bdx = bdx < 0 ? -bdx : bdx;
                            bdy = bdy < 0 ? -bdy : bdy;
                            const float bias =
                                    p.bias_factor * (c / float(task.fanout)) * (float(bdx + bdy) / float(task.hpwl));
                            const float gv = gu + c * hist_c * pres + bias + branch_extra;
                            if (v == sink) {
                                atomic_min_explicit((device atomic_ulong *)best, pack(gv, uint(ei)), RELAXED);
                                continue;
                            }
                            const float f = gv + h_est(p, vx, vy, sx, sy);
                            if (f >= prune2)
                                continue;
                            // rejection against the (step-start) table value; a
                            // tree wire (seed) keeps its existing driver
                            const int sv = table_find(lookup, mask, p.max_probe, uint(v));
                            const ulong cur = sv >= 0 ? vals[sv] : EMPTY_VAL;
                            const ulong nv = pack(gv, uint(ei));
                            if (sv >= 0 && (pack_lo(cur) == NONE_SLOT || nv >= cur))
                                continue;
                            int pos = inc(st.cand_n);
                            if (pos < cap) {
                                cand[pos] = pack(gv, uint(v));
                                cand_par[pos] = uint(ei) | (pack_gbits(nv) < pack_gbits(cur) ? IMPROVES_G : 0u);
                            } else {
                                st_(st.fail, FAIL_PILE);
                            }
                        }
                    }
                }
                SYNC();
                if (tid == 0) {
                    if (ld(st.cand_n) > cap) {
                        st_(st.cand_n, cap);
                        st_(st.fail, FAIL_PILE);
                    }
                }
                SYNC();

                // phase B1: apply the candidates as a minimum, so the new
                // table state is the elementwise minimum over a set that did
                // not depend on thread timing
                const int cn = ld(st.cand_n);
                for (int i = tid; i < cn; i += BLOCK) {
                    const uint v = pack_lo(cand[i]);
                    bool ins;
                    int sv = table_insert(keys, mask, p.max_probe, v, ins);
                    if (sv < 0) {
                        st_(st.fail, FAIL_PROBE);
                        continue;
                    }
                    if (ins) {
                        int cnt = inc(st.inserted);
                        if (cnt > (cap / 5) * 3)
                            st_(st.fail, FAIL_LOAD);
                    }
                    const ulong nv = pack(pack_g(cand[i]), cand_par[i] & ~IMPROVES_G);
                    atomic_min_explicit((device atomic_ulong *)&vals[sv], nv, RELAXED);
                }
                SYNC();

                // phase B2: re-queue a wire only when g itself improved, once,
                // from the candidate that won it. A better parent at equal g
                // updates the table but must not create a frontier entry.
                for (int i = tid; i < cn; i += BLOCK) {
                    const uint par = cand_par[i];
                    if (!(par & IMPROVES_G))
                        continue;
                    const ulong e = cand[i];
                    const int sv = table_find(lookup, mask, p.max_probe, pack_lo(e));
                    if (sv < 0 || vals[sv] != pack(pack_g(e), par & ~IMPROVES_G))
                        continue;
                    int pos = inc(st.far_n);
                    if (pos < cap)
                        far[pos] = e;
                    else
                        st_(st.fail, FAIL_PILE);
                }
                SYNC();
                if (tid == 0) {
                    st.steps++;
                    st_(st.cand_n, 0);
                    if (ld(st.far_n) > cap) {
                        st_(st.far_n, cap);
                        st_(st.fail, FAIL_PILE);
                    }
                    if (ld(st.fail))
                        st.decision = 1;
                }
                SYNC();
            }

            // 4. result and path
            if (tid == 0) {
                const ulong bv = *best;
                const int fail = ld(st.fail);
                ArcResult r;
                r.expanded = ld(st.expanded);
                r.steps = st.steps;
                r.reason = fail;
                r.path_off = task.path_off + st.path_pos;
                r.path_len = 0;
                r.cost = 0.0f;
                if (st.reached) {
                    // An earlier arc already reaches the sink: routed, with no
                    // new wires. A searched path would give the sink a second
                    // parent, and later arcs would seed from wires the host
                    // never binds.
                    r.status = ARC_OK;
                    r.reason = FAIL_NONE;
                } else if (fail) {
                    r.status = ARC_OVERFLOW;
                } else if (bv == EMPTY_VAL) {
                    r.status = ARC_NO_PATH;
                } else {
                    r.status = ARC_OK;
                    r.cost = pack_g(bv);
                    uint edge = pack_lo(bv);
                    int pos = st.path_pos;
                    int cur = sink;
                    float attach_delay = 0.0f;
                    while (true) {
                        if (pos >= task.path_cap) {
                            r.status = ARC_PATH_FULL;
                            break;
                        }
                        const int par = edge_src(out_off, la.n_wires, int(edge));
                        PathEntry pe;
                        pe.wire = cur;
                        pe.parent = par;
                        pe.edge = int(edge);
                        pe.delay = 0.0f;
                        paths[task.path_off + pos] = pe;
                        pos++;
                        const int pslot = table_find(lookup, mask, p.max_probe, uint(par));
                        if (pslot < 0) {
                            r.status = ARC_OVERFLOW; // cannot happen: parent was inserted
                            break;
                        }
                        const uint gp = pack_lo(vals[pslot]);
                        if (gp == NONE_SLOT) {
                            attach_delay = sdelay[pslot];
                            break; // par is a seed (already in the tree)
                        }
                        cur = par;
                        edge = gp;
                    }
                    if (r.status == ARC_OK) {
                        // upstream base delay of every new wire, attach point first
                        float d = attach_delay;
                        for (int k = pos - 1; k >= st.path_pos; k--) {
                            d += edge_cost[paths[task.path_off + k].edge];
                            paths[task.path_off + k].delay = d;
                        }
                        r.path_len = pos - st.path_pos;
                        st.path_pos = pos;
                    }
                }
                results[task.arc_off + a] = r;
            }
            SYNC();
        }
    }
}
