# Experimental routed placement feedback

Mistral can record a fresh placement and its complete final analogue timing,
then evaluate bounded placement, LUT-pin and routing alternatives against that
reference. These controls are disabled by default. They do not enable timing
exceptions, change the requested clock frequency or qualify missing IO timing.

The native entry point is `--routed-placement-record DIR`. Full runs require
fresh ordinary HeAP/GPU placement and routing, `--rbf`, `--write` and `--report`.
Loaded checkpoints, FES scaffold/cart modes, Python hooks and timing-failure
overrides are rejected. Every output directory must be new. Recording alone
does not select a different placement or route.

For example, record a reference using the original inputs and constraints:

```sh
gpu-run --phase-lease --label routed-reference -- nextpnr-mistral \
  --device 5CSEBA6U23I7 --json synth.json --qsf pins.qsf --sdc clocks.sdc \
  --freq 100 --seed 1 --placer heap --router gpu \
  --write parent/routed.json --report parent/timing.json --rbf parent/core.rbf \
  --routed-placement-record parent/native --routed-placement-record-calibration
```

`placement.json` records the fresh original placement. `routed-identity.json`
records the final native identity, including graph, parameters, port states,
placement, strengths, clocks, timing settings, ordering and RNG state. The
normal native BUF-to-FF route-through pattern has an architecture-owned
logical identity. Other cells and connections cannot be normalized away.

`endpoints.json` retains every known endpoint/clock-pair row, static and
inactive records, unsupported observations and primitive-clock checks.
`model.json` exports measured routing observations. The separate
`--routed-placement-record-calibration` opt-in also records the complete native
trees, background resources, reservations and historical calibration needed
for exact execution-state comparisons. This is diagnostic data, not a timing
model for previously unobserved resources.

`status.json` records final native analysis. `completion.json` is written only
after the routed JSON, RBF and report have completed; it retains the actual
error counts and strict timing result. A completed run can still fail timing
or have incomplete coverage. Consumers must retain those distinctions.

## Bounded proposals and routing preservation

`--routed-placement-propose MODEL` requires `--no-route` and explicit
`--routed-placement-trials 1..64` and `--routed-placement-candidates 1..4`.
Predicted timing ranks independent legal alternatives; it cannot authorize a
keeper. `--routed-placement-plan FILE` validates a cumulative plan against its
original and immediate-parent identities during a fresh complete flow.

Separate opt-ins support whole logic/register groups, displaced-LUT site
search, exact data-tree preservation and protected unchanged sink branches.
They preserve cluster, region, native capacity, physical pin and ownership
contracts. Unavailable resources, incomplete searches or identity mismatches
reject the proposal. Search exhaustion and exceptions restore the parent.

The Python entry points `mistral/routed_placement.py`,
`mistral/routed_placement_refine.py` and
`mistral/routed_placement_displaced.py` orchestrate matched full routes. Their
retention checks require complete matching coverage, nondecreasing setup at
every timed row, safe related hold, unchanged clock contracts and no unrelated
path regression. A keeper needs at least 20 ps WNS gain, or equal WNS and at
least 20 ps TNS gain. Rejection retains the entire original bundle.

## Post-route refinement

Independent CLI budgets enable late ordinary-LUT moves, private L5/LUT6 pin
swaps, individual data-net trials and related bounded LUT placement/pin
alternatives. All default to zero and are never enabled by saved JSON
settings. The `--post-route-*` controls require a fresh full recorded route;
they cannot be combined with checkpoint, proposal or preserved-route modes.
Every accepted alternative needs actual full analogue endpoint and clock
checks. Unrelated trees, physical cell connections and binding strengths stay
fixed; failed trials restore their parent exactly.

`--post-route-lut-branch-plan FILE` evaluates a compound proposal after those
refinements. Its version-1 JSON has exactly five keys:

- `version`: integer `1`.
- `parent`: the matching reference's complete `routed-identity.json` object.
- `route_state`: its `model.json` field `cohort_route_model.route_costs`.
- `move`: exactly `cell`, `from_bel`, `to_bel` and `strength`.
- `branch_trials`: an integer from 0 through 16.

Use the same binary, inputs, seed, constraints and preceding refinement options
for the parent and candidate. The complete parent and route/cost state must
match the live fresh context before any proposal is attempted. The move must
be an ordinary L2–L5 LUT in an otherwise empty, FF-free ALM, targeting a
different empty ALM in the same tile and half. Native legality remains
authoritative.

The intermediate LUT move is a planning state. Native bounded Dijkstra rebuilds
its input prefixes and output forest, then enumerates branches on the measured
critical path. Each branch is compared with the original complete reference,
including the unchanged 20 ps threshold. The planning move alone is never
retained. Failed branches restore the exact planning-state RBF and timing; a
failed complete proposal restores the original placement and every route.
Normal strict bitstream signoff still runs afterwards.

Native route-through insertion can leave an FF's cached DATAIN pointing at
BUF.A after its actual input has been connected to BUF.Q. The compound stage
refreshes only this exactly recognized architecture-owned relationship and
recounts LAB inputs. It requires unchanged actual RBF, full endpoint inventory,
graph, pins, LAB controls, reservations and calibration. Whole-design legality
and target eligibility run after that proof; the native 42-input limit is
unchanged.

Detailed before/cache-refreshed/seed/trial/after evidence is under
`native/lut-branch/`. `native/post-route-lut-branch.json` records the final
decision. The cumulative `post-route-lut-moves.json` receipt binds accepted
LUT-only changes to the native final identity. An expired budget or rejected
proposal supplies no new accepted result.

The [validation record](validation/mistral-routed-lut-branch-2026-10-27.md)
separates integrated-branch checks from the earlier source-bound i386
measurement. Neither constitutes production or hardware timing closure.
