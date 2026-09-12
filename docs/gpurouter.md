# GPU router

`--router gpu` is a connection-based negotiated-congestion router whose
shortest-path searches run on a GPU, followed by a timing-repair phase that
re-routes the worst-slack connections at pure delay. It is built on ROCm/HIP
or CUDA and falls back to a sequential host implementation of the same
algorithm when nextpnr is built without a device backend or no GPU is
present.

Design goals: at least router1-class timing with router2-class run time,
and results that do not depend on GPU scheduling (two runs with the same
seed produce the same routing).

## Building

```sh
cmake . -B build -DARCH=mistral -DMISTRAL_ROOT=... -DGPU_ROUTER=HIP \
    -DCMAKE_HIP_ARCHITECTURES="gfx1100;gfx1201"
cmake --build build
```

`GPU_ROUTER` accepts `OFF` (default; CPU reference backend only), `HIP`
(ROCm; `find_package(hip)` under `/opt/rocm` or `ROCM_PATH`) and `CUDA`
(`find_package(CUDAToolkit)`). The device code lives in
`common/route/gpu/gpuroute_kernel.cuh` and is compiled once per backend from
`gpuroute_hip.hip` or `gpuroute_cuda.cu` into a static library with the
plain C++ interface in `gpuroute_backend.h`; the rest of nextpnr is compiled
by the normal C++ compiler. The CUDA build shares the kernel source through
`gpuroute_compat.h`; it has only been compiled and run on ROCm so far.

`--router gpu` is available for the `mistral` and `generic` architectures.
Adding it to another architecture is a one-line dispatch in that
architecture's `Arch::route()` plus an entry in `availableRouters`.

## Command line

| Option | Effect |
| --- | --- |
| `--router gpu` | select the router |
| `--gpu-device N` | GPU index (default: the device with the most compute units) |
| `--gpu-cpu` | run the same algorithm on the sequential host backend |
| `--gpu-perf` | per-iteration timing, slack and backend statistics |
| `--gpu-batches N` | bounding-box-disjoint batches per iteration (default 12) |
| `--gpu-opt name=value` | set any `gpurouter/<name>` tuning setting (repeatable) |
| `--tmg-ripup` | timing-driven rip-up inside the negotiation loop, as for router2 |

Tuning settings (see `GpuRouterCfg` in `common/route/gpurouter.h`):

| Setting | Default | Meaning |
| --- | --- | --- |
| `bbMargin/x`, `bbMargin/y` | 3 | net bounding-box padding, tiles |
| `initCurrCongWeight`, `histCongWeight`, `currCongWeightMult` | 0.5, 1.0, 2.0 | congestion schedule (router2 values) |
| `estimateWeight` | 1.25 | A* heuristic weight |
| `biasCostFactor` | 0.25 | pull towards the net centroid |
| `seedDelayWeight`, `seedDelayFloor` | 1.0, 0.0 | a sink attaches to a tree wire at cost `weight·(floor + (1−floor)·crit)·upstream delay` |
| `critWeightFloor`, `critWeightMode`, `critExponent` | 0.05, 0, 2 | criticality weight `max(floor, 1−crit²)` (mode 0) or `(1−crit)^exp` (mode 1) |
| `loadPenalty`, `pipAdder` | 0, 0 | optional analogue-model approximations (ns per existing branch, ns per pip) |
| `repairRounds`, `repairSlack`, `repairBand`, `repairImproveRounds` | 10, 0 ps, 300 ps, 4 | timing repair (below) |
| `repairDisplace`, `repairDisplaceMargin` | true, 0 ps | a stuck repair may displace frozen arcs with at least this much more slack |
| `cpuLaneNets` | 0 | batches of at most this many nets run on the host backend (0: never) |
| `tmgRipupPatience` | 8 | iterations without progress before `--tmg-ripup` gives up |
| `expandK`, `expandDiv` | 256, 0 | frontier entries expanded per step |
| `smallSlots`, `smallBits`, `largeSlots`, `largeBits` | 384, 16, 4, 22 | device scratch: concurrent nets and log2 table size per lane |
| `maxBatches`, `maxIter` | 12, 2000 | batches per iteration, iteration limit |

## Algorithm

### Host side (`common/route/gpurouter.cc`)

1. **Graph flattening.** All wires are numbered tile-major and the pips are
   stored as a CSR adjacency list with a per-pip base cost
   (`getPipDelay + getWireDelay + epsilon`, in ns; negative when the
   architecture forbids the pip). Each wire carries its notional location,
   a "reserved for net" index and an "unavailable" flag derived from what is
   already bound in the Arch (dedicated global clock routing is bound with
   `STRENGTH_LOCKED` before the router runs). The Cyclone V graph
   (2.74 M wires, 27 M pips) flattens in about 0.2 s on the development
   machine using all cores and uploads in under 0.1 s.
2. **Estimate model.** `Arch::estimateDelay` is sampled on random wire pairs
   and fitted to `a·|dx| + b·|dy|` so the device heuristic reproduces the
   architecture's estimate without calling into it.
3. **Per-net trees.** The host keeps the authoritative routing tree of every
   net (wire → driving pip, use count per arc, upstream base delay) and the
   per-wire occupancy, history and reservation, as router2 does. Pre-routed
   arcs are detected and kept; wires only one net can ever use are reserved
   for it (router2's `find_all_reserved_wires`).
4. **Negotiation.** Each iteration rips up every arc that is unrouted, shares
   an overused wire, or (with `--tmg-ripup`) fails slack, and collects the
   affected nets in criticality order. Nets are greedily grouped into
   batches whose bounding boxes do not overlap; the last batch accepts
   overlap so a handful of chip-spanning nets do not serialise the
   iteration. Nets in one batch are routed concurrently on the device from
   the same congestion snapshot, and the returned paths are applied in a
   fixed order, so concurrency changes nothing but which nets saw each
   other's present congestion. History and present-congestion weights
   follow router2 (`hist += overuse·histCongWeight`,
   `curr_cong_weight += currCongWeightMult` per iteration, bounding boxes
   expand every third failure).
5. **Lanes.** A batch first runs in the *small* lane (many concurrent nets,
   2^16-entry hash tables). Arcs whose search outgrows that scratch retry in
   the *large* lane (few concurrent nets, 2^22 entries, enough for the whole
   device), still inside their bounding box, and finally without a bounding
   box, mirroring router2's retry-without-box.
6. **Timing repair.** After negotiation converges, the timing analyser is
   run and every arc whose setup slack is below `repairSlack` (and, while
   nothing fails, within `repairBand` of the worst slack, for up to
   `repairImproveRounds` rounds) is re-routed *one net at a time* at pure
   delay: no history, present-congestion or bias terms, and the existing
   tree costed by its upstream delay so the sink takes the true
   minimum-delay route from the source. The wires of a repaired arc are then
   reserved for its net and the arc is frozen, so the negotiation loop that
   follows moves the displaced non-critical arcs instead of the repaired
   ones. A repaired arc may only attach to tree wires whose upstream path
   is free of other nets' reservations, which is what makes two frozen arcs
   unable to deadlock on a shared wire. Reservations made by freezing are
   *soft*: when a repair finds no route at all, it is retried ignoring other
   nets' soft reservations, and if every frozen arc it would displace has at
   least `repairDisplaceMargin` more slack, those arcs are unfrozen and left
   to the negotiation loop; otherwise the route is given up. Rounds continue
   while the worst slack improves; the best state is snapshotted and
   restored if a later round made it worse.
7. **Binding.** The trees are bound into the Arch with `bindWire`/`bindPip`;
   anything the Arch rejects is negotiated again. router1 then runs as the
   final legality check exactly as after router2.

### Device side (`common/route/gpu/gpuroute_kernel.cuh`)

One thread block (256 threads) routes one net; the arcs of a net are routed
in sequence so later arcs can attach to earlier ones. Per arc:

1. A private open-addressing hash table `wire → (g, parent wire)` is
   cleared. Keys use double hashing; the value is one 64-bit word so it can
   be updated with `atomicMin`.
2. The tree wires are inserted as seeds with `g = crit·upstream delay`
   (scaled by `seedDelayWeight`/`seedDelayFloor`) and pushed onto the
   frontier pile.
3. Each *step* compacts the frontier (dropping stale and pruned entries and
   tracking the f-range), histograms `f = g + w·h` into 128 bins, chooses
   the threshold that selects about `expandK` entries, and expands those in
   parallel. Expansion is two-phase: all edges are relaxed against the table
   as it was at the start of the step and recorded as candidates; then the
   candidates are applied with `atomicMin`. A node is re-queued only when
   its `g` strictly improves, and a tree wire never changes its driver. The
   search ends when the sink has been reached and no frontier entry has `f`
   below the best sink cost, or the frontier is empty.
4. Thread 0 walks the parent chain back to the first tree wire and writes
   `(wire, parent, upstream delay)` triples to the task's output region;
   later arcs of the same task use them as additional seeds.

The per-wire cost is router2's: `base · hist(w) · present(w) + bias`, with
`hist` and `present` scaled by the arc's criticality weight
`max(0.05, 1 − crit²)` and `bias` pulling towards the net centroid. In
repair mode the criticality weight is 0 and the bias factor 0, so the cost
is pure base delay. With `expandK = 1` the kernel is plain A*; larger values
are the usual K-best parallel A* trade of extra expansions for parallel
work; the termination rule is A*'s, so path quality matches A* with the
same (inadmissible, weighted) heuristic.

### Determinism

The result of a run does not depend on GPU thread scheduling:

- expansion candidates are computed from the table state at the start of a
  step and applied afterwards, so the table after each step is an
  elementwise minimum over a scheduling-independent set;
- `(g, parent wire)` is one 64-bit value, so equal-cost ties resolve to the
  smallest parent wire index;
- nodes are re-queued only on strict `g` improvement, so the frontier is a
  set, not an order-dependent multiset;
- the expansion threshold is chosen from counts over the de-duplicated
  frontier, and pruning uses a best-cost snapshot taken at step boundaries;
- the host applies paths, updates congestion and repairs timing in a fixed
  order.

Two runs with the same seed and options produce the same routing checksum
(`mistral/tests/gpurouter/qor.py` checks this). The remaining exception is
a scratch overflow occurring in one run and not the other; the table load
limit (60 %) and probe limit (512, with double hashing) make that unlikely,
and an overflow only moves the arc to the large lane.

## Memory

Per concurrent net the small lane uses about `2^16 · 56 B` ≈ 3.7 MB and the
large lane `2^22 · 56 B` ≈ 235 MB (defaults 384 and 4 concurrent nets, about
2.3 GB in total), plus the graph (about 230 MB for Cyclone V). All of it is
allocated once per run.

## Validation

`mistral/tests/gpurouter/qor.py` routes a fixture with `--router gpu` twice
and with `--router router2`, checks convergence, signoff timing and
reproducibility, and prints router time and Fmax side by side. Run it on the
retained M10K netlist without arguments, or point it at a larger synthesis
output such as the misteross FES ZX81, Pong or ColecoVision cores.

Results on the development machine (Radeon RX 7900 XTX, ROCm 7.14, 24-core
host, nextpnr fd862a2c base, misteross fixtures as of 2026-09-12). Fmax is
the post-route table-model value the final report prints; router time is
the router's own timer (the router2 column for the two multi-PLL cores is
router2 alone, before Mistral's flow retries it with router1).

| Design (seed 1) | Clock | router2 | router1 | gpu |
| --- | --- | --- | --- | --- |
| M10K mixed-width fixture | 50 MHz | 377.79 | 467.95 | 467.95 |
| FES Pong | core.game.clk (74.25 MHz) | 77.16 | 86.07 | 82.67 |
| FES ZX81 | clk_sys (52 MHz) | 50.18 (fail) | 57.50 | 58.17 |
| FES ZX81 | pixel_clk (74.25 MHz) | 85.48 | 124.38 | 151.86 |
| FES ColecoVision | clk_sys (52 MHz) | 50.83 (fail) | 56.40 | 58.08 |
| FES ColecoVision | pixel_clk (74.25 MHz) | 71.80 (fail) | 90.83 | 107.55 |

| Design (seed 3) | Clock | router1 | gpu |
| --- | --- | --- | --- |
| FES ZX81 | clk_sys / pixel_clk | 55.52 / 127.84 | 56.14 / 153.96 |
| FES ColecoVision | clk_sys / pixel_clk | 57.96 / 96.66 | 58.04 / 105.30 |

| Design | router2 time | gpu time (incl. repair) | whole-run wall: router2 flow / router1 / gpu |
| --- | --- | --- | --- |
| M10K fixture | 0.15 s | 0.76 s | 3.6 s / 3.5 s / 4.3 s |
| FES Pong | 0.53 s | 1.51 s | 5.4 s / 6.7 s / 6.4 s |
| FES ZX81 | 3.43 s (+ router1 retry) | 4.75 s | 41.2 s / 34.1 s / 15.8 s |
| FES ColecoVision | 17.60 s (+ router1 retry) | 4.90 s | 63.0 s / 58.9 s / 13.6 s |

Those Fmax values are the per-pip table model, which is what the final
report uses when no bitstream is written. With `--rbf` Mistral configures
the bitstream first and reports its analogue interconnect model instead;
the misteross-sealed FES ZX81 package routed by the host backend reports
55.04 / 114.94 MHz that way, against 54.18 / 97.59 MHz for the previous
router1 seal.

The sequential CPU reference backend (`--gpu-cpu`) reaches similar Fmax
(ZX81 57.77 / 121.26, ColecoVision 58.45 / 91.73) in 5.1 s and 8.8 s of
router time, so on these small cores the GPU mostly buys the whole-run wall
time and the headroom for larger designs; the QoR comes from the algorithm.
On these fixtures the device is far from busy: the negotiation tail and the
one-net-at-a-time repair run a handful of blocks.

## Limitations and future work

- Constant-value nets (`NetInfo::constant_value`) and the resource API
  (`getResourceKeyForPip`) are not supported; Mistral uses neither.
- Only one GPU is used. The batch structure would allow a second device to
  take alternate batches with the same deterministic apply order.
- The tail of the negotiation (a few nets fighting over a few wires) and
  the one-net-at-a-time repair leave most of the GPU idle. `cpuLaneNets`
  routes such tiny batches on the host backend instead; it saves about a
  second on the ZX81 core but its exact A* picks different equal-cost paths
  than the K-best kernel, which moved Fmax both ways on the fixtures (Pong
  +3.7 MHz, ColecoVision pixel clock −18 MHz), so it is off by default.
- The graph is flattened on every run. Caching the CSR on disk keyed by the
  device would remove most of the fixed setup cost for small designs.
- Pong shows router1 still ahead by about 4 %; the repair phase stops when
  the worst slack stops improving and some repairs fail because the wires
  they need are already reserved, so a smarter repair order or unfreezing
  is the next step.
