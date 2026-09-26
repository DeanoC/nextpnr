# GPU router

`--router gpu` is a connection-based negotiated-congestion router whose
shortest-path searches run on a GPU, followed by a timing-repair phase that
re-routes the worst-slack connections at pure delay. It is built on ROCm/HIP
or CUDA and falls back to a sequential host implementation of the same
algorithm when nextpnr is built without a device backend or no GPU is
present. The host backend is a scalar transcription of the kernel (same
K-best steps, cost expressions, tie-breaking and per-lane capacity limits),
so a machine without a GPU produces the same routing as one with a GPU.

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
| `congestionStallIters`, `congestionStallBoost`, `congestionStallBoostMax` | 20, 1.5, 50 | if the overused-wire count sets no new minimum for this many iterations, `currCongWeightMult` is scaled by this factor (compounding every further `congestionStallIters` iterations with no improvement, capped at `congestionStallBoostMax`) until a new minimum is reached. A genuine hard conflict (no legal alternative route at all) does not respond to any cost weight; `maxIter` remains the backstop for that case |
| `estimateWeight` | 1.25 | A* heuristic weight |
| `repairEstimateWeight` | = `estimateWeight` | A* heuristic weight of the pure-delay searches (timing repair, peer groups, candidate generation) |
| `biasCostFactor` | 0.25 | pull towards the net centroid |
| `seedDelayWeight`, `seedDelayFloor` | 1.0, 0.0 | a sink attaches to a tree wire at cost `weight·(floor + (1−floor)·crit)·upstream delay` |
| `critWeightFloor`, `critWeightMode`, `critExponent` | 0.05, 0, 2 | criticality weight `max(floor, 1−crit²)` (mode 0) or `(1−crit)^exp` (mode 1) |
| `loadPenalty`, `pipAdder` | 0, 0 | optional analogue-model approximations (ns per existing branch, ns per pip) |
| `repairRounds`, `repairSlack`, `repairBand`, `repairImproveRounds` | 10, 0 ps, 300 ps, 4 | timing repair (below) |
| `repairDisplace`, `repairDisplaceMargin` | true, 0 ps | a stuck repair may displace frozen arcs with at least this much more slack |
| `repairCongWeight` | 1.0 | present-congestion weight used when re-routing a same-band peer group (0 disables). History cost is ignored in that pass, so an occupied wire costs `(1 + occ * weight)` times delay. |
| `analogueRounds`, `analogueSlack`, `analogueRipSlack`, `analoguePrior` | 3, 0 ps, 300 ps, 1.25 | Mistral analogue signoff repair (below); `analogueRounds=0` disables it |
| `analogueCandidates`, `analogueCandidateRounds`, `analogueCandidateArcs` | 4, 2, 200 | analogue-scored candidate selection (below): routes generated per failing sink, passes before each full re-route, failing sinks tried per pass; `analogueCandidates=0` disables it |
| `analogueCandidateGain`, `analogueCandidateFanout`, `analogueCandidatePrior` | 20 ps, 64, 1.0 | a candidate must raise the net's worst sink slack by this much to be kept; nets with more sinks are left alone; delay prior for unobserved pips while searching candidates |
| `analogueRipNets` | 64 | nets ripped per re-route round, worst analogue slack first (0: every net with an arc below `analogueRipSlack`) |
| `analogueRevert`, `analogueRevertMargin`, `analogueRestoreMargin` | true, 20 ps, 1000 ps | after a re-route, give nets whose worst sink got slower by the margin their old route back where possible; abandon a round this far below the best routing and restore that instead |
| `candidateMargin`, `candidateUnbounded`, `candidateExpandK` | 8, true, = `expandK` | candidate searches use the net's bounding box widened by this many tiles; whether the two primary candidates may fall back to the search without a box; frontier entries expanded per step (they run one net at a time, so 2048 makes a pass about a third faster, with different routes) |
| `cpuLaneNets` | 0 | batches of at most this many nets run on the host backend (0: never) |
| `repairVerify`, `repairVerifyArcs` | false, 300 | diagnostic: re-run the first arc of the first N bounded pure-delay repair tasks as an exact Dijkstra on the host and report how often and by how much the K-best weighted-A* search misses the minimum-delay route (slow: seconds per search; candidate searches are not verified) |
| `tmgRipupPatience` | 8 | iterations without progress before `--tmg-ripup` gives up |
| `expandK`, `expandDiv` | 256, 0 | frontier entries expanded per step |
| `smallSlots`, `smallBits`, `largeSlots`, `largeBits` | 384, 16, 4, 22 | device scratch: concurrent nets and log2 table size per lane |
| `maxBatches`, `maxIter` | 12, 2000 | batches per iteration, iteration limit |

## Algorithm

### Host side (`common/route/gpurouter.cc`)

1. **Graph flattening.** All wires are numbered tile-major (ties within a
   tile by the wire's own hash, not by the order the Arch enumerates
   wires, so the numbering and the search's tie-breaking do not change
   with the order a chip database lists its nodes in) and the pips are
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
   an overused wire (unless it was frozen by timing repair, below), or
   (with `--tmg-ripup`) fails slack, and collects the
   affected nets in criticality order. Nets are greedily grouped into
   batches whose bounding boxes do not overlap; the last batch accepts
   overlap so a handful of chip-spanning nets do not serialise the
   iteration. Nets in one batch are routed concurrently on the device from
   the same congestion snapshot, and the returned paths are applied in a
   fixed order, so concurrency changes nothing but which nets saw each
   other's present congestion. History and present-congestion weights
   follow router2 (`hist += overuse·histCongWeight`,
   `curr_cong_weight += currCongWeightMult` per iteration, bounding boxes
   expand every third failure). If the overused-wire count sets no new
   minimum for `congestionStallIters` iterations, `currCongWeightMult` is
   scaled by `congestionStallBoost` (compounding every further
   `congestionStallIters` iterations of no improvement) until a new minimum
   is reached, so a small number of nets stuck swapping the same wire do
   not stall the whole design for a very long time under the plain
   additive schedule. Before the weight is boosted, frozen arcs that share
   an overused wire are unfrozen, all but the most critical per wire: a
   frozen arc is never ripped up, so two of them on one wire (which the
   peer-group and displacement passes can leave behind) would otherwise
   stall until `maxIter`. Once the overused set is already small, a frozen
   arc that is the only frozen user of an overused wire is unfrozen too:
   otherwise the movable net is ripped and put back on that wire forever.
   Likewise an arc that finds no route at all, even
   without a bounding box, is routed once more ignoring soft reservations
   and the frozen arcs it then displaces are unfrozen whatever their
   slack, instead of the run failing. When four or fewer wires have stayed
   overused for 30 iterations, or the boost is already at its ceiling on a
   plateau of at most eight wires, the stuck nets are routed one at a time
   with soft reservations ignored. Frozen arcs on a path that avoids the
   overuse are unfrozen, whatever their slack. If that does not clear the
   wires, a re-negotiation started by timing repair restores the legal
   pre-repair routing and stops, instead of running on to `maxIter`. The
   initial negotiation still fails the run, because there is no earlier
   legal routing to restore.
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
   to the negotiation loop; otherwise the failed arc is kept for a
   *peer-group* pass. A delay-only attempt that still cannot route restores
   that net's previous legal tree so re-negotiation does not abort on an
   unrouted arc. That pass groups a failed repair with frozen arcs in
   the same slack band (`repairBand`) that occupy its minimum-delay wires,
   rips the group, and re-routes it worst-slack-first at pure delay plus
   `repairCongWeight` present congestion (history cost ignored) so later
   peers can share a short trunk at 2× cost instead of being blocked by a
   hard reservation. The group is frozen only after every member has been
   attempted. Each round's worst slack is the design WNS over every sink,
   including frozen arcs; a freeze that still fails the slack request is
   unfrozen and re-repaired, while a freeze that already meets the request
   is left reserved even if it sits inside the Fmax improvement band. Rounds continue
   while that WNS improves. If a round does not improve and the WNS still
   fails the request, one reversed-order retry is attempted from the best
   snapshot; the best state is restored if a later round made it worse.
7. **Binding.** Every net's router-made routing is unbound, then the trees
   are bound into the Arch with `bindWire`/`bindPip`; anything the Arch
   rejects is negotiated and timing-repaired again. (Unbinding and binding
   one net at a time, as before, refused the wires a net had legitimately
   taken from a net the negotiation moved but whose turn had not come, and
   the re-route of the refused net then ended on a detour.) router1 then
   runs as the final legality check exactly as after router2.
8. **Analogue signoff repair (Mistral).** The table in `mistral/delay.cc`
   has one delay per wire type, but signoff uses Mistral's analogue model,
   whose delay depends on the physical line and tap, the configured load
   and the input slope. On the FES ColecoVision core the table
   underestimates a routed arc by 140 ps on average and by more than 1 ns
   at the 99th percentile (H3/H6 lines near the M10K columns reach 500-700
   ps against a 226/275 ps table entry), so routes that pass the table
   miss signoff. After routing, `Arch::route` configures the bitstream and
   times the design with the analogue model (arcs are simulated in parallel
   and cached, which also speeds up the final signoff). If a clock has less
   than `analogueSlack` slack, every routed pip's analogue delay is
   recorded; `getPipDelay` then returns the recorded delay, or for an
   unobserved pip the table scaled by the observed per-type ratio times
   `analoguePrior` (so repairs prefer wires of known delay). Nets with an
   arc below `analogueRipSlack` analogue slack are ripped up and the GPU
   router runs again with `repairSlack` set to the same margin; the other
   nets start out routed and are negotiated like everything else (reserving
   their wires left the ripped nets with hard conflicts on local lines that
   no cost could resolve). This repeats for
   up to `analogueRounds` rounds and the routing with the best analogue
   slack is kept. A design that already meets signoff is unchanged.

   During a re-route round the nets that stay bound keep the analogue
   delay observed for them (`Arch::getArcDelayOverride` answers from the
   observation cache for unchanged arcs, and the entries of the ripped nets
   are dropped), so the router's timing analysis, criticality and repair
   decisions mix exact delays for what it keeps with the calibrated table
   for what it moves, instead of the calibrated table's pessimistic prior
   for everything.
9. **Analogue candidate selection (Mistral).** The per-type table and the
   analogue model rank routes differently, and the K-best search's choice
   between equal-cost paths decides how heavily loaded trunks are (see
   Validation). So before a signoff miss is answered with a full re-route,
   `Arch::analogue_candidate_pass` takes the failing sinks worst slack
   first (`analogueCandidateArcs` of them) and, for each, asks
   `GpuCandidateRouter` for up to `analogueCandidates` materially
   different pure-delay routes, all inside the net's bounding box: the
   router's own choice (attach anywhere on the tree), a route from the
   source only, then ones avoiding every multi-tile wire of the earlier
   candidates while that still finds new routes, then one avoiding each
   multi-tile wire of the route the arc has now; for a net with several
   sinks also the whole tree rebuilt with the failing sink first and the
   others after it by slack, and a star with every sink routed from the
   source (greedy sink-by-sink routing leaves the critical sink attached
   to a tree built for the others; the star is electrically clean at the
   cost of wires), while the sink is still below `analogueCandidates`.
   The route the net already has and repeats are dropped. The candidate router is a
   `GpuRouter` set up from what the Arch has bound (every other net's wires
   are reserved for it), so a candidate is legal against the rest of the
   design. All sinks of a pass are searched together, one variant per
   launch, so the searches fill the device (a pass on the ZX81 takes about
   5 s where one net at a time took 15-50 s); the wires a variant must
   avoid are passed to the kernel as blocked seeds of that task alone, so
   one net's avoidance does not constrain another's search in the same
   launch. Candidates of different nets may compete for a free wire; the
   loser is refused when it is bound and counted in the log. Each candidate is bound in the Arch, the analogue simulator's
   routing muxes are updated for the changed pips, and *every* sink of the
   net is simulated, because a new branch changes the load on shared
   upstream segments; the score is the net's worst estimated sink slack
   (old slack plus old minus new analogue delay). The best candidate is
   kept if it beats the current route by `analogueCandidateGain`,
   otherwise the routing and mux state are restored. A pass costs about
   two seconds on the FES cores. Up to `analogueCandidateRounds` passes run,
   each followed by a full analogue re-time, before each re-route; a pass
   that changes nothing goes straight on to the re-route.

   With a libmistral that defines `MISTRAL_RNODE_UNLINK` (DeanoC/mistral
   master since 2c28969d implements the long-declared `rnode_unlink`;
   the fork's `main` requires 7ed06e21 or later), a mux the net no
   longer drives is returned to its default. The pinned library
   lacks it, so there the mux is parked on an input no net drives with
   `rnode_link`, which likewise takes it off the load of the wire the net
   left; on the ZX81 both give the same candidate decisions. This state is
   only ever simulated, the bitstream is rebuilt from scratch before it is
   written.

   A re-route rips at most `analogueRipNets` nets, the worst by analogue
   slack (0: every net with an arc below `analogueRipSlack`); a few dozen
   perturb the routes the next candidate passes choose from at a fraction
   of the cost of several hundred, and on the ZX81 both reach the same
   slack. After a re-route the nets whose worst sink got slower by
   `analogueRevertMargin` are given their old route back where the wires
   are still free (`analogueRevert`), and a round that ends
   `analogueRestoreMargin` below the best routing so far is abandoned and
   the best routing restored before the next round. Both are safety nets:
   re-route rounds used to land 3-6 ns below the state they started from,
   which turned out to be the final binding refusing wires (below), and
   with that fixed neither triggers often.

### Device side (`common/route/gpu/gpuroute_kernel.cuh`)

One thread block (256 threads) routes one net; the arcs of a net are routed
in sequence so later arcs can attach to earlier ones. Per arc:

1. A private open-addressing hash table `wire → (g, parent edge)` is
   cleared. Keys use double hashing; the value is one 64-bit word so it can
   be updated with `atomicMin`. The parent is the CSR index of the pip, not
   just the wire it comes from, so architectures with several pips between
   one pair of wires bind the pip the search actually chose.
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
   `(wire, parent, edge, upstream delay)` entries to the task's output region;
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
- `(g, parent edge)` is one 64-bit value, so equal-cost ties resolve to the
  smallest parent edge index (edges are ordered by source wire, then pip);
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
and with a reference router (`router2`, or `--reference router1`), writing
a bitstream each time so the reported Fmax is the post-bitstream analogue
model that signoff uses, with the delay-table Fmax of the same routing in
parentheses (`--no-rbf` reports the table only). It checks convergence, a
routing checksum, a non-empty clock set (`--expect-clock` names required
clocks), signoff timing and reproducibility, and prints router time and
both Fmax views side by side, plus the analogue repair rounds of the GPU
flow. When Mistral's flow retries a marginal router2 result with router1,
the script reports router2's own Fmax and labels the final numbers as
router1's. Run it on the retained M10K netlist without arguments, or point
it at a larger synthesis output such as the misteross FES ZX81, Pong or
ColecoVision cores with `--extra-arg` for the placer settings of their
recipes. `--arc-dump` writes the per-hop table and analogue delays of
every routed arc.

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
the bitstream first and reports its analogue interconnect model instead.
The analogue result varies noticeably between Yosys builds of the same
core: on one FES ZX81 synthesis every router fails the 52 MHz clock under
the analogue model except this one (router1 with rip-up 49.64 MHz, router2
then router1 47.71 MHz, GPU router 59.36 MHz), while on another all pass.

The host backend (`--gpu-cpu`) produces the identical routing (checked
net by net on the M10K fixture and by Fmax on the ZX81 fixtures) in about
3.5 times the router time of the GPU (ZX81 17.7 s against 4.8 s), still
faster than router1 there. The K-best stepping itself matters for quality:
an earlier exact-A* host backend that only matched the cost model routed
the same synthesis to 49.9 MHz under Mistral's analogue model where the
K-best search reaches 59.4 MHz, because the two prefer different equal-cost
paths and the analogue model punishes heavily loaded trunks. On these
fixtures the device is far from busy: the negotiation tail and the
one-net-at-a-time repair run a handful of blocks.

### Analogue signoff with candidate selection

Post-bitstream analogue Fmax (what signoff uses), same machine, FES
fixtures as of 2026-09-25 (the ZX81 synthesis is the one from the 52 MHz
investigation, which the table model passes at 51.7 MHz and the analogue
model fails at 45.4 MHz), comparing the branch at `bedc5ab1` with the
candidate-selection flow described above. "Best round" means the analogue
repair loop kept an earlier round because later ones were worse. Wall
time is the whole run including placement and bitstream generation.

| Design (seed) | Clock | `bedc5ab1` | with candidate selection | wall before / after |
| --- | --- | --- | --- | --- |
| M10K fixture (1) | 50 MHz | 468.16 | 441.50 | 4.5 s / 5.2 s |
| FES Pong (1) | core.game.clk (74.25) | 86.75 | 86.75 | 6.2 s / 6.4 s |
| FES ColecoVision (1) | clk_sys (52) / pixel_clk (74.25) | 53.03 / 77.77 (check 51.28 fail, passed in round 2) | 57.48 / 77.77 (passes at the check) | 24.4 s / 21.9 s |
| FES ZX81 (1) | clk_sys (52) | 45.37 fail (best round) | 51.39 fail (−0.23 ns) | 48 s / 85 s |
| FES ZX81 (2) | clk_sys (52) | 49.31 fail (best round) | **52.37 pass** (round 6) | 34 s / 63 s |
| FES ZX81 (3) | clk_sys (52) | 48.50 fail (best round) | 48.56 fail (−1.36 ns) | 41 s / 100 s |

On the ZX81 the candidate passes do the closing (seed 1: −2.75 → −0.23
ns over seven rounds of passes of about 2 s each, seed 2: −1.20 → +0.13
ns); the re-route rounds diversify the routes the next passes choose
from. The numbers above are the defaults after the binding
fix; before it, runs that were wrecked by a refused binding and then
recovered by the passes sometimes closed seed 1 (52.72 MHz) and sometimes
ended at 48 MHz, which is the spread to expect from a heuristic flow, not
a property of the defaults. The analogue simulation of the candidates is
well under a second per pass. The ColecoVision now passes at the first analogue check
because of the delay-table entries measured with `--arc-dump` (see
`getPipDelayTable`), before any repair. `qor.py` on the ZX81 seed 1
reproduces the checksum across two GPU runs and reports the router2 flow
(retried with router1) at 41.65 MHz analogue on the same fixture. Seed 3
shows the limit: its critical path has 14 ns of routing over some twenty
hops and few of its sinks have a materially different route inside their
box; that is a placement problem, not one more route search.

### Is the search leaving delay on the table?

`repairVerify` re-runs the first arc of the first 300 bounded pure-delay
repair tasks of a run as an exact Dijkstra on the host, from the same
tree, state and box, and compares the costs (only a task's first arc is
comparable: later arcs are seeded from the paths chosen before them,
which differ between the two searches). On the FES ZX81 (seed 1) 7 of
300 (2.3 %) K-best weighted-A* routes were longer than the minimum-delay
route, by 141 ps on average and 259 ps at most, in the first route; in
the three analogue re-route rounds of the same run (calibrated table,
122-126 searches each) 0.8-4.9 % were longer, mean 143-469 ps, worst
1.17 ns. The search is close to exact under the scalar table, so a
better lookahead or heuristic weight would not change results; what the
router optimises (the table against the analogue model) matters, not
how well. `repairEstimateWeight=1.0` is the knob to try if a design
shows more.

### Calibration experiments

The analogue arc dumps suggest the per-hop error grows by 15-30 ps per
extra branch a wire drives, which `loadPenalty` (ns charged per existing
branch when a sink attaches to a loaded tree wire) and `pipAdder` (ns
added to every pip) can approximate in the search cost. Measured on the
FES ZX81 (seeds 1-3), ColecoVision and Pong against the defaults, final
analogue slack in ns:

| Setting | ZX81 1 | ZX81 2 | ZX81 3 | Coleco | Pong |
| --- | --- | --- | --- | --- | --- |
| defaults | −0.26 | +0.27 | −1.36 | +0.61 | +1.94 |
| `loadPenalty=0.02` | −0.22 | +0.17 | −1.21 | +0.65 | +1.94 |
| `loadPenalty=0.04` | −0.68 | +0.01 | −1.24 | +0.65 | +1.84 |
| `pipAdder=0.05` | −0.29 | −0.08 | −1.48 | +0.23 | +2.05 |

The first route does move (seed 1's initial analogue check improves from
−2.75 ns to −1.63 ns with `loadPenalty=0.04`), but after candidate
selection the results are within the seed-to-seed spread, so the
defaults stay at 0. A per-type, per-load delay table used by the timing
analysis itself, rather than a search-cost approximation, remains open.

## Limitations and future work

- Constant-value nets (`NetInfo::constant_value`) and the resource API
  (`getResourceKeyForPip`) are not supported; Mistral uses neither.
- Only one GPU is used. The batch structure would allow a second device to
  take alternate batches with the same deterministic apply order.
- The tail of the negotiation (a few nets fighting over a few wires) and
  the one-net-at-a-time repair leave most of the GPU idle. `cpuLaneNets`
  routes such tiny batches on the host backend instead, with identical
  results now that the host backend mirrors the kernel, but the scalar
  K-best steps are slower than a one-block launch on these fixtures (ZX81
  6.2 s against 4.8 s), so it is off by default.
- A route whose new wires do not fit the task's path output region comes
  back as `ARC_PATH_FULL` and is retried with the region scaled up eight
  times per attempt, bounded by the wire count.
- The graph is flattened on every run. Caching the CSR on disk keyed by the
  device would remove most of the fixed setup cost for small designs.
- The full re-route round is a lottery: it explores, but often lands
  3-6 ns below the state it started from, and the nets it hurts mostly
  cannot get their old wires back. The candidate passes plateau after two
  or three rounds on a given state (no sink has a better single-arc
  alternative), so the next gain needs moves the passes do not make yet:
  rebuilding a whole net's tree with a different sink order, or joint
  candidates for all nets of the worst path.
- `analogue_candidate_pass` handles one sink per net per pass; a net with
  several failing sinks needs several passes.
- Pong remains about 4 % behind router1 on the documented seed-1 fixture.
  Repair decisions use the design WNS, including frozen sinks, so freeze-first
  cannot hide a failing critical path. A reversed-order retry still runs when
  a round does not improve and that WNS fails the request.
