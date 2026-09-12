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
 *  GPU device backend: one thread block per net, block-parallel bucketed A*
 *  ("near/far" delta-stepping with an A* heuristic) per connection.
 *
 *  Included by gpuroute_hip.hip (ROCm / HIP) and gpuroute_cuda.cu (CUDA).
 *
 *  Algorithm outline (per block, per arc of the task):
 *    1. Clear a private open-addressing hash table  wire -> (g, parent edge).
 *    2. Insert every wire of the net's current routing tree as a seed with
 *       g = 0 (connection-based routing: the sink may attach anywhere) and
 *       put the seeds on the frontier pile.
 *    3. Repeat: compact the frontier (dropping stale and pruned entries),
 *       build a histogram of f = g + h and pick the threshold that selects
 *       about expand_k entries, expand those in parallel and push improved
 *       children back onto the frontier. Stop when the frontier is empty or
 *       its lowest f is not below the best sink cost found so far.
 *    4. Thread 0 walks the parent chain and writes (wire, parent, edge) entries.
 *
 *  This is a K-best parallel A*: with expand_k = 1 it is ordinary A*, larger
 *  values trade a small amount of path optimality for parallel work.
 *
 *  Determinism: each step relaxes edges against the table state at the
 *  start of the step and only then applies the candidates, and the
 *  (g, parent edge) pair is stored as one 64-bit value updated with
 *  atomicMin, so equal-cost ties resolve to the smallest parent edge index
 *  (edges are ordered by source wire, then pip order)
 *  and the table after a step is an elementwise minimum over a
 *  scheduling-independent set; the expansion set of each step is chosen from
 *  deterministic counts over the de-duplicated frontier, and pruning uses a
 *  snapshot of the best sink cost taken at step boundaries. The result therefore does not depend on GPU scheduling
 *  (except in the rare event of a scratch overflow, which is retried in the
 *  large lane).
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpuroute_backend.h"
#include "gpuroute_compat.h"

namespace gpuroute {

namespace {

constexpr int BLOCK = 256;
constexpr uint32_t EMPTY_KEY = 0xFFFFFFFFu;
constexpr uint32_t NONE_SLOT = 0xFFFFFFFFu; // "parent" marker of a seed wire
constexpr unsigned long long EMPTY_VAL = ~0ull;
constexpr uint32_t INF_BITS = 0x7f800000u;
constexpr int NBINS = 128; // histogram bins used to pick each step's expansion set
constexpr int NPILES = 4;  // far, far2, the per-step expansion set, and the candidate list

struct DevGraph
{
    int32_t n_wires;
    const int32_t *out_off;
    const int32_t *out_dst;
    const float *edge_cost;
    const int16_t *wx;
    const int16_t *wy;
    int32_t *reserved;
    const uint8_t *flags;
    int32_t *occ;
    float *hist;
};

struct Scratch
{
    uint32_t *keys;            // slots * cap
    unsigned long long *vals;  // slots * cap
    float *sdelay;             // slots * cap, upstream base delay of seed entries
    float *sload;              // slots * cap, existing branch count of seed entries
    unsigned long long *piles; // slots * NPILES * cap
    uint32_t *cand_par;        // slots * cap, parent edge of each candidate
    int cap;
};

struct BlockState
{
    int near_n, far_n, far2_n, cand_n;
    int fail;
    int inserted;
    int expanded;
    int steps;
    int path_pos;
    int far_idx;
    int decision; // 0 continue, 1 stop
    float binw;
    uint32_t thr_bits;
    uint32_t prune_bits;
    uint32_t minf_bits, maxf_bits;
    unsigned long long best;
};

__device__ __forceinline__ uint32_t f2u(float f) { return __float_as_uint(f); }
__device__ __forceinline__ float u2f(uint32_t u) { return __uint_as_float(u); }
__device__ __forceinline__ unsigned long long pack(float g, uint32_t lo)
{
    return ((unsigned long long)f2u(g) << 32) | (unsigned long long)lo;
}
__device__ __forceinline__ float pack_g(unsigned long long v) { return u2f((uint32_t)(v >> 32)); }
__device__ __forceinline__ uint32_t pack_gbits(unsigned long long v) { return (uint32_t)(v >> 32); }
__device__ __forceinline__ uint32_t pack_lo(unsigned long long v) { return (uint32_t)v; }

__device__ __forceinline__ uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

__device__ __forceinline__ uint32_t hash_step(uint32_t x)
{
    // second hash for double hashing; odd so every slot is visited
    x ^= x >> 13;
    x *= 0x5bd1e995U;
    x ^= x >> 15;
    return (x << 1) | 1u;
}

__device__ __forceinline__ int table_find(const uint32_t *keys, int mask, int max_probe, uint32_t w)
{
    uint32_t i = hash32(w) & mask;
    const uint32_t step = hash_step(w);
    for (int p = 0; p < max_probe; p++) {
        uint32_t k = keys[i];
        if (k == w)
            return (int)i;
        if (k == EMPTY_KEY)
            return -1;
        i = (i + step) & mask;
    }
    return -1;
}

// Returns the slot for wire w, inserting it if needed; -1 if the probe
// limit is hit. *inserted is set when this call created the entry.
__device__ __forceinline__ int table_insert(uint32_t *keys, int mask, int max_probe, uint32_t w, bool *inserted)
{
    uint32_t i = hash32(w) & mask;
    const uint32_t step = hash_step(w);
    for (int p = 0; p < max_probe; p++) {
        uint32_t k = keys[i];
        if (k == w) {
            *inserted = false;
            return (int)i;
        }
        if (k == EMPTY_KEY) {
            uint32_t old = atomicCAS(&keys[i], EMPTY_KEY, w);
            if (old == EMPTY_KEY) {
                *inserted = true;
                return (int)i;
            }
            if (old == w) {
                *inserted = false;
                return (int)i;
            }
        }
        i = (i + step) & mask;
    }
    return -1;
}

__device__ __forceinline__ float h_est(const RouteParams &p, int x, int y, int sx, int sy)
{
    int dx = x - sx, dy = y - sy;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    return p.est_weight * (p.est_x * (float)dx + p.est_y * (float)dy);
}

// Source wire of CSR edge e: the largest wire whose row starts at or before e
__device__ __forceinline__ int edge_src(const DevGraph &g, int e)
{
    int lo = 0, hi = g.n_wires - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (g.out_off[mid] <= e)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

__global__ void __launch_bounds__(BLOCK) route_kernel(DevGraph g, RouteParams p, const TaskDesc *tasks, int ntasks,
                                                      const ArcDesc *arcs, const int32_t *seeds,
                                                      const float *seed_delay, const float *seed_load, Scratch sc,
                                                      ArcResult *results, PathEntry *paths)
{
    __shared__ BlockState st;
    __shared__ int hist[NBINS];
    const int tid = threadIdx.x;
    const int cap = sc.cap;
    const int mask = cap - 1;
    uint32_t *keys = sc.keys + (size_t)blockIdx.x * cap;
    unsigned long long *vals = sc.vals + (size_t)blockIdx.x * cap;
    float *sdelay = sc.sdelay + (size_t)blockIdx.x * cap;
    float *sload = sc.sload + (size_t)blockIdx.x * cap;
    unsigned long long *pilebase = sc.piles + (size_t)blockIdx.x * NPILES * (size_t)cap;
    unsigned long long *near = pilebase + 2 * (size_t)cap;
    unsigned long long *cand = pilebase + 3 * (size_t)cap;
    uint32_t *cand_par = sc.cand_par + (size_t)blockIdx.x * cap;

    for (int t = blockIdx.x; t < ntasks; t += gridDim.x) {
        const TaskDesc task = tasks[t];
        if (tid == 0)
            st.path_pos = 0;
        __syncthreads();

        for (int a = 0; a < task.arc_cnt; a++) {
            const ArcDesc arc = arcs[task.arc_off + a];
            const int sink = arc.sink;
            const int sx = g.wx[sink], sy = g.wy[sink];

            // 1. clear the table
            for (int i = tid; i < cap; i += BLOCK) {
                keys[i] = EMPTY_KEY;
                vals[i] = EMPTY_VAL;
            }
            if (tid == 0) {
                st.near_n = 0;
                st.far_n = 0;
                st.far2_n = 0;
                st.cand_n = 0;
                st.fail = 0;
                st.inserted = 0;
                st.expanded = 0;
                st.steps = 0;
                st.far_idx = 0;
                st.decision = 0;
                st.minf_bits = INF_BITS;
                st.maxf_bits = 0;
                st.prune_bits = INF_BITS;
                st.best = EMPTY_VAL;
            }
            __syncthreads();

            // 2. seeds: the net tree plus the wires added by earlier arcs of this task
            {
                const int nseed = task.tree_cnt + st.path_pos;
                unsigned long long *far = pilebase + (size_t)st.far_idx * cap;
                const float seed_scale =
                        p.seed_delay_weight * (p.seed_delay_floor + (1.0f - p.seed_delay_floor) * arc.crit);
                for (int i = tid; i < nseed; i += BLOCK) {
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
                    bool ins;
                    int s = table_insert(keys, mask, p.max_probe, (uint32_t)w, &ins);
                    if (s < 0) {
                        st.fail = FAIL_PROBE;
                        continue;
                    }
                    if (!ins)
                        continue;
                    atomicAdd(&st.inserted, 1);
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
                    sload[s] = ld;
                    int pos = atomicAdd(&st.far_n, 1);
                    if (pos < cap)
                        far[pos] = pack(g0, (uint32_t)w);
                    else
                        st.fail = FAIL_PILE;
                }
            }
            __syncthreads();
            if (tid == 0) {
                if (st.far_n > cap)
                    st.far_n = cap;
                if (st.fail)
                    st.decision = 1;
            }
            __syncthreads();

            // 3. main loop: each step expands the ~expand_k frontier entries
            //    with the lowest f = g + h, all children go back to the frontier
            while (st.decision == 0) {
                unsigned long long *far = pilebase + (size_t)st.far_idx * cap;
                unsigned long long *far2 = pilebase + (size_t)(st.far_idx ^ 1) * cap;
                const int fn = st.far_n;
                const float prune = u2f(st.prune_bits);

                // pass 1: drop stale and pruned entries, track the f range
                for (int i = tid; i < fn; i += BLOCK) {
                    unsigned long long e = far[i];
                    const uint32_t v = pack_lo(e);
                    const uint32_t gbits = pack_gbits(e);
                    int sv = table_find(keys, mask, p.max_probe, v);
                    if (sv < 0 || pack_gbits(vals[sv]) != gbits)
                        continue;
                    const float f = u2f(gbits) + h_est(p, g.wx[v], g.wy[v], sx, sy);
                    if (f >= prune)
                        continue;
                    atomicMin(&st.minf_bits, f2u(f));
                    atomicMax(&st.maxf_bits, f2u(f));
                    int pos = atomicAdd(&st.far2_n, 1);
                    if (pos < cap)
                        far2[pos] = e;
                    else
                        st.fail = FAIL_PILE;
                }
                __syncthreads();
                if (tid == 0) {
                    const bool have_best = st.best != EMPTY_VAL;
                    const float bestg = have_best ? pack_g(st.best) : u2f(INF_BITS);
                    const float minf = u2f(st.minf_bits);
                    if (st.fail || st.far2_n == 0 || st.minf_bits == INF_BITS || (have_best && minf >= bestg)) {
                        st.decision = 1;
                    } else {
                        const float range = u2f(st.maxf_bits) - minf;
                        st.binw = range > 1e-6f ? range / (float)NBINS : 0.0f;
                        if (st.far2_n > cap)
                            st.far2_n = cap;
                    }
                }
                for (int i = tid; i < NBINS; i += BLOCK)
                    hist[i] = 0;
                __syncthreads();
                if (st.decision != 0)
                    break;

                // pass 2: histogram of f over the compacted frontier
                const int fn2 = st.far2_n;
                const float minf = u2f(st.minf_bits);
                const float binw = st.binw;
                if (binw > 0.0f) {
                    for (int i = tid; i < fn2; i += BLOCK) {
                        unsigned long long e = far2[i];
                        const uint32_t v = pack_lo(e);
                        const float f = pack_g(e) + h_est(p, g.wx[v], g.wy[v], sx, sy);
                        int b = (int)((f - minf) / binw);
                        b = b < 0 ? 0 : (b >= NBINS ? NBINS - 1 : b);
                        atomicAdd(&hist[b], 1);
                    }
                }
                __syncthreads();
                if (tid == 0) {
                    float thr = u2f(INF_BITS);
                    if (binw > 0.0f) {
                        int cum = 0;
                        int want = p.expand_k;
                        if (p.expand_div > 0 && fn2 / p.expand_div > want)
                            want = fn2 / p.expand_div;
                        for (int b = 0; b < NBINS; b++) {
                            cum += hist[b];
                            if (cum >= want) {
                                thr = minf + (float)(b + 1) * binw;
                                break;
                            }
                        }
                    }
                    st.thr_bits = f2u(thr);
                    st.near_n = 0;
                    st.far_n = 0;
                }
                __syncthreads();

                // pass 3: split the frontier into this step's expansion set and the rest
                {
                    const float thr = u2f(st.thr_bits);
                    for (int i = tid; i < fn2; i += BLOCK) {
                        unsigned long long e = far2[i];
                        const uint32_t v = pack_lo(e);
                        const float f = pack_g(e) + h_est(p, g.wx[v], g.wy[v], sx, sy);
                        if (f < thr) {
                            int pos = atomicAdd(&st.near_n, 1);
                            if (pos < cap)
                                near[pos] = e;
                            else
                                st.fail = FAIL_PILE;
                        } else {
                            int pos = atomicAdd(&st.far_n, 1);
                            if (pos < cap)
                                far[pos] = e;
                            else
                                st.fail = FAIL_PILE;
                        }
                    }
                }
                __syncthreads();
                if (tid == 0) {
                    if (st.near_n > cap)
                        st.near_n = cap;
                    if (st.far_n > cap)
                        st.far_n = cap;
                    st.far2_n = 0;
                    st.minf_bits = INF_BITS;
                    st.maxf_bits = 0;
                    st.prune_bits = (st.best != EMPTY_VAL) ? pack_gbits(st.best) : INF_BITS;
                    if (st.fail)
                        st.decision = 1;
                }
                __syncthreads();
                if (st.decision != 0)
                    break;

                // expansion pass, phase A: relax every edge of the expansion
                // set against the table state at the start of the step and
                // record candidates; nothing is written to the table yet
                {
                    const int nn = st.near_n;
                    const float prune2 = u2f(st.prune_bits);
                    for (int i = tid; i < nn; i += BLOCK) {
                        unsigned long long e = near[i];
                        const uint32_t u = pack_lo(e);
                        const float gu = pack_g(e);
                        atomicAdd(&st.expanded, 1);
                        // branching off a tree wire that already drives other
                        // branches loads it further; charge for that
                        float branch_extra = 0.0f;
                        if (p.load_penalty > 0.0f) {
                            const int su = table_find(keys, mask, p.max_probe, u);
                            if (su >= 0 && pack_lo(vals[su]) == NONE_SLOT)
                                branch_extra = p.load_penalty * sload[su];
                        }
                        const int e0 = g.out_off[u], e1 = g.out_off[u + 1];
                        for (int ei = e0; ei < e1; ei++) {
                            const int v = g.out_dst[ei];
                            float c = g.edge_cost[ei];
                            if (c < 0.0f)
                                continue;
                            c += p.pip_adder;
                            const int vx = g.wx[v], vy = g.wy[v];
                            if (p.use_bb && (vx < task.bb_x0 || vx > task.bb_x1 || vy < task.bb_y0 || vy > task.bb_y1))
                                continue;
                            if (g.flags[v] & WIRE_UNAVAILABLE)
                                continue;
                            const int rsv = g.reserved[v];
                            if (rsv != -1 && (rsv & ~RESERVED_SOFT) != task.net &&
                                !(p.ignore_soft && (rsv & RESERVED_SOFT)))
                                continue;
                            const float hist_c = 1.0f + arc.crit_weight * (g.hist[v] - 1.0f);
                            const float pres = 1.0f + (float)g.occ[v] * p.curr_cong_weight * arc.crit_weight;
                            int bdx = vx - task.cx, bdy = vy - task.cy;
                            bdx = bdx < 0 ? -bdx : bdx;
                            bdy = bdy < 0 ? -bdy : bdy;
                            const float bias =
                                    p.bias_factor * (c / (float)task.fanout) * ((float)(bdx + bdy) / (float)task.hpwl);
                            const float gv = gu + c * hist_c * pres + bias + branch_extra;
                            if (v == sink) {
                                atomicMin(&st.best, pack(gv, (uint32_t)ei));
                                continue;
                            }
                            const float f = gv + h_est(p, vx, vy, sx, sy);
                            if (f >= prune2)
                                continue;
                            // rejection against the (step-start) table value; a
                            // tree wire (seed) keeps its existing driver
                            const int sv = table_find(keys, mask, p.max_probe, (uint32_t)v);
                            if (sv >= 0) {
                                const unsigned long long cur = vals[sv];
                                if (pack_lo(cur) == NONE_SLOT || pack(gv, (uint32_t)ei) >= cur)
                                    continue;
                            }
                            int pos = atomicAdd(&st.cand_n, 1);
                            if (pos < cap) {
                                cand[pos] = pack(gv, (uint32_t)v);
                                cand_par[pos] = (uint32_t)ei;
                            } else {
                                st.fail = FAIL_PILE;
                            }
                        }
                    }
                }
                __syncthreads();
                if (tid == 0) {
                    if (st.cand_n > cap) {
                        st.cand_n = cap;
                        st.fail = FAIL_PILE;
                    }
                }
                __syncthreads();

                // phase B: apply the candidates with atomicMin, so the new
                // table state is the elementwise minimum over a set that did
                // not depend on thread timing
                {
                    const int cn = st.cand_n;
                    for (int i = tid; i < cn; i += BLOCK) {
                        unsigned long long e = cand[i];
                        const uint32_t v = pack_lo(e);
                        const float gv = pack_g(e);
                        bool ins;
                        int sv = table_insert(keys, mask, p.max_probe, v, &ins);
                        if (sv < 0) {
                            st.fail = FAIL_PROBE;
                            continue;
                        }
                        if (ins) {
                            int cnt = atomicAdd(&st.inserted, 1);
                            if (cnt > (cap / 5) * 3)
                                st.fail = FAIL_LOAD;
                        }
                        const unsigned long long nv = pack(gv, cand_par[i]);
                        const unsigned long long old = atomicMin(&vals[sv], nv);
                        // Re-queue only when g itself improves. A better
                        // parent at equal g updates the table but must not
                        // create a second frontier entry, or the frontier
                        // would depend on the order the candidates landed.
                        if (pack_gbits(nv) < pack_gbits(old)) {
                            int pos = atomicAdd(&st.far_n, 1);
                            if (pos < cap)
                                far[pos] = e;
                            else
                                st.fail = FAIL_PILE;
                        }
                    }
                }
                __syncthreads();
                if (tid == 0) {
                    st.steps++;
                    st.cand_n = 0;
                    if (st.far_n > cap) {
                        st.far_n = cap;
                        st.fail = FAIL_PILE;
                    }
                    if (st.fail)
                        st.decision = 1;
                }
                __syncthreads();
            }

            // 4. result and path
            if (tid == 0) {
                ArcResult r;
                r.expanded = st.expanded;
                r.steps = st.steps;
                r.reason = st.fail;
                r.path_off = task.path_off + st.path_pos;
                r.path_len = 0;
                r.cost = 0.0f;
                if (st.fail) {
                    r.status = ARC_OVERFLOW;
                } else if (st.best == EMPTY_VAL) {
                    r.status = ARC_NO_PATH;
                } else {
                    r.status = ARC_OK;
                    r.cost = pack_g(st.best);
                    uint32_t edge = pack_lo(st.best);
                    int pos = st.path_pos;
                    int32_t cur = sink;
                    float attach_delay = 0.0f;
                    while (true) {
                        if (pos >= task.path_cap) {
                            r.status = ARC_PATH_FULL;
                            break;
                        }
                        const int32_t par = edge_src(g, (int)edge);
                        PathEntry pe;
                        pe.wire = cur;
                        pe.parent = par;
                        pe.edge = (int32_t)edge;
                        pe.delay = 0.0f;
                        paths[task.path_off + pos] = pe;
                        pos++;
                        const int pslot = table_find(keys, mask, p.max_probe, (uint32_t)par);
                        if (pslot < 0) {
                            r.status = ARC_OVERFLOW; // cannot happen: parent was inserted
                            break;
                        }
                        const uint32_t gp = pack_lo(vals[pslot]);
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
                            PathEntry &pe = paths[task.path_off + k];
                            d += g.edge_cost[pe.edge];
                            pe.delay = d;
                        }
                        r.path_len = pos - st.path_pos;
                        st.path_pos = pos;
                    }
                }
                results[task.arc_off + a] = r;
            }
            __syncthreads();
        }
    }
}

__global__ void update_state_kernel(int32_t *occ, float *hist, int32_t *reserved, int count, const int32_t *wires,
                                    const int32_t *nocc, const float *nhist, const int32_t *nres)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    int w = wires[i];
    occ[w] = nocc[i];
    hist[w] = nhist[i];
    reserved[w] = nres[i];
}

#define GPU_CHECK(call)                                                                                                \
    do {                                                                                                               \
        hipError_t err__ = (call);                                                                                     \
        if (err__ != hipSuccess)                                                                                       \
            throw std::runtime_error(std::string("GPU error: ") + hipGetErrorString(err__) + " at " #call);            \
    } while (0)

template <typename T> struct DevBuf
{
    T *ptr = nullptr;
    size_t count = 0;
    ~DevBuf() { release(); }
    void release()
    {
        if (ptr)
            (void)hipFree(ptr);
        ptr = nullptr;
        count = 0;
    }
    void alloc(size_t n)
    {
        release();
        if (n == 0)
            return;
        GPU_CHECK(hipMalloc((void **)&ptr, n * sizeof(T)));
        count = n;
    }
    void ensure(size_t n)
    {
        if (n > count)
            alloc(std::max(n, count + count / 2));
    }
    void upload(const T *src, size_t n)
    {
        ensure(n);
        if (n)
            GPU_CHECK(hipMemcpy(ptr, src, n * sizeof(T), hipMemcpyHostToDevice));
    }
    void download(T *dst, size_t n) const
    {
        if (n)
            GPU_CHECK(hipMemcpy(dst, ptr, n * sizeof(T), hipMemcpyDeviceToHost));
    }
};

struct Lane
{
    DevBuf<uint32_t> keys;
    DevBuf<unsigned long long> vals;
    DevBuf<float> sdelay, sload;
    DevBuf<unsigned long long> piles;
    DevBuf<uint32_t> cand_par;
    int slots = 0;
    int cap = 0;
    void alloc(int nslots, int bits)
    {
        slots = nslots;
        cap = 1 << bits;
        keys.alloc((size_t)slots * cap);
        vals.alloc((size_t)slots * cap);
        sdelay.alloc((size_t)slots * cap);
        sload.alloc((size_t)slots * cap);
        piles.alloc((size_t)slots * NPILES * (size_t)cap);
        cand_par.alloc((size_t)slots * cap);
    }
    Scratch scratch() const
    {
        Scratch s;
        s.keys = keys.ptr;
        s.vals = vals.ptr;
        s.sdelay = sdelay.ptr;
        s.sload = sload.ptr;
        s.piles = piles.ptr;
        s.cand_par = cand_par.ptr;
        s.cap = cap;
        return s;
    }
};

class DeviceBackend : public Backend
{
  public:
    explicit DeviceBackend(int device) : device_(device) {}

    bool init(const GraphData &graph, int small_slots, int small_bits, int large_slots, int large_bits,
              std::string &error) override
    {
        try {
            GPU_CHECK(hipSetDevice(device_));
            hipDeviceProp_t prop;
            GPU_CHECK(hipGetDeviceProperties(&prop, device_));
            name_ = std::string(GPUROUTE_BACKEND_NAME) + ":" + prop.name;
            n_wires_ = graph.n_wires;
            out_off_.upload(graph.out_off, (size_t)graph.n_wires + 1);
            out_dst_.upload(graph.out_dst, (size_t)graph.n_edges);
            edge_cost_.upload(graph.edge_cost, (size_t)graph.n_edges);
            wx_.upload(graph.wire_x, graph.n_wires);
            wy_.upload(graph.wire_y, graph.n_wires);
            reserved_.upload(graph.wire_reserved, graph.n_wires);
            flags_.upload(graph.wire_flags, graph.n_wires);
            occ_.upload(graph.wire_occ, graph.n_wires);
            hist_.upload(graph.wire_hist, graph.n_wires);
            small_.alloc(small_slots, small_bits);
            large_.alloc(large_slots, large_bits);
            GPU_CHECK(hipDeviceSynchronize());
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
        return true;
    }

    void update_wire_state(size_t count, const int32_t *wires, const int32_t *occ, const float *hist,
                           const int32_t *reserved) override
    {
        if (count == 0)
            return;
        auto t0 = std::chrono::steady_clock::now();
        upd_wires_.upload(wires, count);
        upd_occ_.upload(occ, count);
        upd_hist_.upload(hist, count);
        upd_res_.upload(reserved, count);
        int blocks = (int)((count + 255) / 256);
        update_state_kernel<<<blocks, 256>>>(occ_.ptr, hist_.ptr, reserved_.ptr, (int)count, upd_wires_.ptr,
                                             upd_occ_.ptr, upd_hist_.ptr, upd_res_.ptr);
        GPU_CHECK(hipGetLastError());
        GPU_CHECK(hipDeviceSynchronize());
        stats_.transfer_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    void route(const RouteParams &params, const std::vector<TaskDesc> &tasks, const std::vector<ArcDesc> &arcs,
               const std::vector<int32_t> &seeds, const std::vector<float> &seed_delay,
               const std::vector<float> &seed_load, bool large_lane, std::vector<ArcResult> &results,
               std::vector<PathEntry> &paths) override
    {
        results.assign(arcs.size(), ArcResult());
        size_t total_paths = 0;
        for (auto &t : tasks)
            total_paths = std::max(total_paths, (size_t)t.path_off + (size_t)t.path_cap);
        paths.assign(total_paths, PathEntry{-1, -1, -1, 0.0f});
        if (tasks.empty())
            return;

        auto t0 = std::chrono::steady_clock::now();
        tasks_.upload(tasks.data(), tasks.size());
        arcs_.upload(arcs.data(), arcs.size());
        seeds_.upload(seeds.data(), seeds.size());
        seed_delay_.upload(seed_delay.data(), seed_delay.size());
        seed_load_.upload(seed_load.data(), seed_load.size());
        results_.ensure(arcs.size());
        paths_.ensure(total_paths);
        GPU_CHECK(hipMemset(results_.ptr, 0, arcs.size() * sizeof(ArcResult)));
        auto t1 = std::chrono::steady_clock::now();
        stats_.transfer_seconds += std::chrono::duration<double>(t1 - t0).count();

        const Lane &lane = large_lane ? large_ : small_;
        int grid = (int)std::min<size_t>(tasks.size(), (size_t)lane.slots);
        DevGraph g;
        g.n_wires = n_wires_;
        g.out_off = out_off_.ptr;
        g.out_dst = out_dst_.ptr;
        g.edge_cost = edge_cost_.ptr;
        g.wx = wx_.ptr;
        g.wy = wy_.ptr;
        g.reserved = reserved_.ptr;
        g.flags = flags_.ptr;
        g.occ = occ_.ptr;
        g.hist = hist_.ptr;
        route_kernel<<<grid, BLOCK>>>(g, params, tasks_.ptr, (int)tasks.size(), arcs_.ptr, seeds_.ptr,
                                      seed_delay_.ptr, seed_load_.ptr, lane.scratch(), results_.ptr, paths_.ptr);
        GPU_CHECK(hipGetLastError());
        GPU_CHECK(hipDeviceSynchronize());
        auto t2 = std::chrono::steady_clock::now();
        stats_.route_seconds += std::chrono::duration<double>(t2 - t1).count();
        stats_.launches++;

        results_.download(results.data(), arcs.size());
        paths_.download(paths.data(), total_paths);
        stats_.transfer_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
        for (auto &r : results) {
            stats_.arcs_routed++;
            stats_.wires_expanded += r.expanded;
        }
    }

    const char *name() const override { return name_.c_str(); }
    const BackendStats &stats() const override { return stats_; }

  private:
    int device_;
    std::string name_;
    int32_t n_wires_ = 0;
    DevBuf<int32_t> out_off_, out_dst_;
    DevBuf<float> edge_cost_;
    DevBuf<int16_t> wx_, wy_;
    DevBuf<int32_t> reserved_;
    DevBuf<uint8_t> flags_;
    DevBuf<int32_t> occ_;
    DevBuf<float> hist_;
    Lane small_, large_;
    DevBuf<TaskDesc> tasks_;
    DevBuf<ArcDesc> arcs_;
    DevBuf<int32_t> seeds_;
    DevBuf<float> seed_delay_, seed_load_;
    DevBuf<ArcResult> results_;
    DevBuf<PathEntry> paths_;
    DevBuf<int32_t> upd_wires_, upd_occ_, upd_res_;
    DevBuf<float> upd_hist_;
    BackendStats stats_;
};

} // namespace

bool device_backend_available()
{
    int n = 0;
    return hipGetDeviceCount(&n) == hipSuccess && n > 0;
}

std::string describe_devices()
{
    int n = 0;
    if (hipGetDeviceCount(&n) != hipSuccess)
        return "";
    std::string s;
    for (int i = 0; i < n; i++) {
        hipDeviceProp_t prop;
        if (hipGetDeviceProperties(&prop, i) != hipSuccess)
            continue;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s%d: %s (%d CUs, %zu MB)", i ? "; " : "", i, prop.name, prop.multiProcessorCount,
                 (size_t)(prop.totalGlobalMem >> 20));
        s += buf;
    }
    return s;
}

std::unique_ptr<Backend> create_device_backend(int device, std::string &error)
{
    int n = 0;
    hipError_t err = hipGetDeviceCount(&n);
    if (err != hipSuccess || n == 0) {
        error = "no GPU device available";
        if (err != hipSuccess)
            error += std::string(" (") + hipGetErrorString(err) + ")";
        return nullptr;
    }
    if (device < 0) {
        int best = 0, best_cu = -1;
        for (int i = 0; i < n; i++) {
            hipDeviceProp_t prop;
            if (hipGetDeviceProperties(&prop, i) != hipSuccess)
                continue;
            if (prop.multiProcessorCount > best_cu) {
                best_cu = prop.multiProcessorCount;
                best = i;
            }
        }
        device = best;
    }
    if (device >= n) {
        error = "requested GPU device index out of range";
        return nullptr;
    }
    return std::unique_ptr<Backend>(new DeviceBackend(device));
}

} // namespace gpuroute
