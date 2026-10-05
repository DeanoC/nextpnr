# Seed racing: RNG and reproducibility audit

Audit date: 2026-10-04. Scope: current branch `codex/seed-racing-foundation-20261004`; source review only. No PNR invocation was made, so none of the four permitted 120-second runs was consumed. In particular, issue #119's Catch/Pong observation is not reproduced here.

## Confirmed call paths

`Context` owns the ordinary deterministic xorshift64* stream. Seed zero aliases the fixed default, and `rngseed()` advances the stream five times (`common/kernel/deterministic_rng.h:33-101`). CLI setup seeds it from loaded context state when present, then from `--seed`, or assigns a `random_device` value for `--randomize-seed`; the resulting **RNG state**, rather than the user's original integer, is stored as the `seed` setting (`common/kernel/command.cc:428-463`, `common/kernel/command.cc:562-575`). Setup runs before JSON import (`common/kernel/command.cc:642-660`), so a route-only `--seed N` establishes the live stream even though imported JSON later replaces the serialized `settings.seed` value. The integer seed must therefore remain an identifier, not an ordered feature.

| Phase | Confirmed use of randomness |
| --- | --- |
| Mistral pack and global routing | No RNG call was found in `mistral/pack.cc`, `mistral/globals.cc`, or the Mistral pack/route dispatch. Their iteration/order is not evidence of seed dependence. |
| Placement | Both SA and default HeAP consume the shared `Context` stream for shuffles, candidate locations, rip-up escape, and acceptance (`common/place/placer1.cc:199,367,432-449,549,708,765-776`; `common/place/placer_heap.cc:736,1295-1313`). Static placement and timing optimization also consume it. Optional parallel refinement instead gives each thread a private, default-initialized `DeterministicRNG` (`common/place/detail_place_core.h:151-157`; `common/place/parallel_refine.cc:64,216-227,318`); it is off by default (`common/place/placer_heap.cc:2167`). |
| GPU graph/heuristic setup | Flattening orders wires by tile then wire hash, and adjacency fill is per-wire (`common/route/gpurouter.cc:177-259`). The estimate fit uses a private stream with constant seed `0x5eed1234`, so it is repeatable and independent of `--seed` (`common/route/gpurouter.cc:799-843`). |
| GPU negotiation | Every initial or repair-triggered `negotiate()` iteration calls `ctx->sorted_shuffle(route_queue)` before a stable criticality sort (`common/route/gpurouter.cc:1385-1419`). This is the GPU router's direct route-only seed entry point. It shares the stream already advanced by placement in a full PNR run; it is not a separate routing seed. |
| GPU timing repair | Failing arcs are selected by setup slack, stably sorted worst-first, optionally reversed after a non-improving round, and handled one net at a time (`common/route/gpurouter.cc:2171-2395`). Candidate and peer searches contain no private random stream. Their subsequent re-negotiation does consume the shared stream, so repair trajectories can still be seed-dependent. Best legal snapshots are restored after regression or a repair plateau. |
| Backend fallback | When no requested device backend exists, the same flattened algorithm uses the sequential CPU reference backend (`common/route/gpurouter.cc:2440-2456`). This selection consumes no RNG; it does not make a distinct random policy. The device kernel resolves equal-cost entries by packed `(cost,parent)` minima and the host applies results in fixed order (`common/route/gpu/gpuroute_kernel.cuh`; `docs/gpurouter.md`, Determinism). This is an implementation claim that still needs repeated-run evidence for #119's exact cohort. |
| Final GPU legality pass | After binding/retry and timing repair, GPU routing invokes router1 (`common/route/gpurouter.cc:2820-2898`). Router1 always advances the shared stream while shuffling setup order and assigning queue tie tags (`common/route/router1.cc:133-146,372-445`); a complete legal GPU tree normally leaves no arcs to route, but missing/inconsistent arcs can be routed there. |
| Other routers/fallback | Router2 shuffles each iteration and seeds private per-thread search RNGs from the shared stream (`common/route/router2.cc:1541-1558,1715-1733`). Mistral may rip up a marginal multi-PLL/M10K router2 result and retry router1 (`mistral/arch.cc:1001-1065`), so the final result then uses the already-advanced shared stream. This cohort must not be mixed with GPU runs. |
| Analogue repair | GPU routing alone calls `analogue_repair()` (`mistral/arch.cc:1066-1075`). Candidate order and ripped-net choice are stable slack/name sorts, but every full repair reroute creates another GPU router whose negotiations consume the same shared stream (`mistral/analogue.cc:465-701`). Thus a final checksum printed by the earlier router1 legality pass can precede analogue route changes. |

Consequently, `--seed` identifies a whole full-flow trajectory. On a genuinely fixed placement it also changes GPU negotiation order directly. It does not control the fixed heuristic-fit samples, and there is no independent supported routing-seed option today. Adding one should be opt-in and compatibility-tested rather than changing existing semantics.

## Placement save/load semantics

The supported mechanism is `--write placed.json` after `--no-route`, followed by `--json placed.json --no-pack --no-place`. At the end of placement Mistral sets module attribute `step=place` and calls `archInfoToAttributes()` (`mistral/arch.cc:980-987`). The generic writer serializes module settings/attributes, all cell parameters/attributes and connections, and net attributes (`json/jsonwrite.cc:109-229`). Physical state is represented as:

- `NEXTPNR_BEL` plus `BEL_STRENGTH` for placed cells;
- `ROUTING` for all currently bound net wires/pips, including fixed or pre-routed resources;
- ordinary cell/net/module attributes and the resolved settings map.

Import restores settings and attributes, then calls `attributesToArchInfo()`; that rebinds BELs and routing at the recorded strengths before Mistral `assignArchInfo()` reconstructs architecture metadata (`frontend/frontend_base.h:280-302`; `common/kernel/basectx.cc:182-246`; `mistral/arch.cc:790-821`). Therefore BEL locations, strength, serialized routes, attributes, and settings have a defined round trip.

This is **not yet a proved clean routing-only checkpoint for general Mistral designs**. Generic JSON does not serialize `CellInfo::pin_data`. Mistral recreates default pin maps on import, while M10Ks explicitly have no default because they require a custom map (`mistral/arch.cc:790-805`). The complete `FES_PINMAP_V1` and `FES_LABSTATE_V1` snapshots are created by `save_fes_pin_maps()` only at the end of routing (`mistral/fes_slot.cc:311-342`; `mistral/arch.cc:1087-1091`) and restored only by the FES scaffold locking path. A routed JSON therefore preserves those maps but also preserves `ROUTING`, route strength/history-visible starting state, and `step=route`; merely changing its seed is not a clean placed checkpoint.

Before fixed-placement racing, add a focused round-trip test containing at least a custom M10K map, an alternate LAB input map, a fixed global/pre-route, folded/inverted pin states, cell/net/module attributes, and non-default router settings. Compare live BEL/pin/route structures before and after reload, then prove that removing only router-made routing leaves the intended fixed resources. Until that passes, use full place-and-route cohorts.

## Final timing authority

GPU negotiation and repair optimize the per-pip table. `Arch::route()` logs that table-model Fmax after routing (`mistral/arch.cc:1077-1087`), but it is not Mistral signoff. When `--rbf` is requested, `signoff_after_route` disables the router1 table-model timing gate; bitstream construction configures the physical mux state, computes analogue arcs, and runs the final skew-aware timing analysis (`mistral/main.cc:121-135,156-163`; `mistral/bitstream.cc:1006-1029`). That final analysis reports every clock and related-clock setup constraint plus hold/min-delay violations; `--timing-allow-fail` changes failures to warnings, not passes (`common/kernel/timing_log.cc:150-333`). A success label therefore requires legal routing, normal completion, all required analogue setup/related-clock checks, and no required hold/min-time failure. Without `--rbf`, reports are table-based only and must be labelled proxy timing.

## Reproducibility status and #119

Confirmed from code and retained tests:

- `mistral/tests/gpurouter/qor.py` repeats each GPU seed and compares the logged routing checksum, but its default fixture is the retained mixed-width M10K design. The repository contains no retained Catch or Pong mapped netlist, constraints, manifest, paired logs, or structural route hashes attributable to issue #119.
- Issue #119 reports that Pong's new-pin producer route reached 84.11 MHz, while re-routing the same mapped input with seed 1 reached 78.09 MHz; the old-pin seed-1 route reportedly repeated at 84.62 MHz. The exact Catch/Pong files live on the named Powerboat path rather than in this checkout, so their input hashes, full environment and final routes could not be compared here. The new-pin difference remains an unresolved observation, not evidence of a GPU scheduling fault.
- A logged checksum alone is insufficient when analogue repair later changes routes. Reproducibility comparison must use the canonical final route plus final analogue per-clock results, with identical mapped JSON and constraints hashes, binary/chip-database identity, argv/resolved settings, backend/device/driver, thread count, and environment allowlist.

Smallest resolving test: obtain the exact #119 Catch/Pong inputs and manifest, run two same-seed replicates on one fixed backend/configuration, and compare final canonical route and analogue timing; separately compare CPU-reference and device backends as different cohorts. Then repeat telemetry off/on. Until that evidence exists, a seed must identify a replicated outcome, not be assumed to identify a deterministic replayable trajectory.

## Tests and blockers

Tests run: source/fixture inspection only; no build and no PNR. Existing automated coverage was read but not rerun. Blockers are (1) absent exact #119 artifacts/manifest, (2) no verified general placed-JSON custom-pin-map round trip, and (3) no same-input replicated final-route evidence on this branch. These block routing-only and deterministic-replay claims, but do not block opt-in telemetry, bounded collection, or offline replay with replicate-aware outcomes.
