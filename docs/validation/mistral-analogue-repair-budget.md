# Bounded Mistral analogue repair

The placement study retained two seed-4 timeouts after analogue timing had
started. Repair saved its best routing, but a nested GPU-router call could
spend the remaining compiler time in congestion negotiation. External process
termination prevented restoration and final timing of that saved routing.

Ordinary repair stops on limits set by its inputs. Wall-clock time is only a
safety limit:

- `analogueRounds` (default 3) caps full re-routes (`reroute_cap`); `0`
  disables repair (`disabled`). Repair-loop timings are also bounded
  internally by `(analogueRounds + 1) * (analogueCandidateRounds + 3)`
  (`round_cap`).
- Optional `--gpu-opt analogueMaxRounds=N` (default `0`, no cap) caps repair
  actions, not timings. Candidate passes and full re-routes count in the
  repair loop (`round_cap` when the cap stops it). Once the target is met,
  `analogueImproveRounds` passes count against the same total; a cap that ends
  them keeps `target_met` as the stop reason, and `improve_passes` shows the
  cut. The cap applies alongside the internal bound, not as the minimum of the
  two; `0` only removes the action cap. With the default candidate settings
  the first action is a candidate pass, so `analogueMaxRounds=1` never
  reaches a re-route. Separate opt-in post-route local experiments keep their
  own trial limits and are not counted.
- `--gpu-opt analogueTimeBudget=SECONDS` (default 3600 s, `0` disables) is a
  cooperative safety limit, so ordinary runs do not stop early under host
  load.

Each run logs `Analogue repair stop condition: reason=... rounds=...
reroutes=... candidate_passes=... improve_passes=... time_budget_truncated=...`.
`--report` JSON carries the same fields under top-level `analogue_repair`;
`timing_summary` is unchanged. `rounds` counts repair-loop timings, including
the initial check. The stop reasons are `disabled`, `target_met`,
`reroute_cap`, `round_cap`, `no_rip_nets`, `reroute_failed`, `time_budget`
(between rounds), `time_budget_candidate`, `time_budget_reroute`,
`time_budget_improve` and `time_budget_post_route`. The last replaces an
untruncated reason when expiry stops or skips a requested opt-in post-route
experiment after ordinary repair has finished.

Initial routing is unchanged. The first analogue timing always completes so
repair has a measured route to retain. Candidate generation, candidate scoring
and nested routing check the budget at safe boundaries. Active backend batches,
graph setup steps and timing analyses finish before cancellation; this is
not a hard deadline for the whole compilation.

Cancelled nested routing unwinds its Context lock and backend state before
the caller restores the best saved routing. Cancelled candidate selection
also restores the temporary search delay prior. Partially changed candidates
are never treated as already timed. Restored routing is configured and timed
again through the existing final analogue setup/hold gate. A requested
optimization margin remains distinct from timing acceptance.

## Validation

Two new unit tests check cancellation inside the locked negotiation loop,
candidate cancellation, invalid budgets and the disabled limit. The 31
existing IO timing regressions also pass.

`mistral/tests/analogue_budget.py` uses a retained passing menu DDR checkpoint
and an unattainable repair target to exercise these cases:

| Case | Result |
| --- | --- |
| Repair disabled | ordinary final timing passes |
| Immediate budget expiry | final timing passes; exact baseline bindings and RBF retained |
| Nested rerouting before expiry | best routing retained or restored; final timing passes; checkpoint reproduces the exact resulting RBF |
| Candidate selection before expiry | best routing retained or restored; final timing passes; checkpoint reproduces the exact resulting RBF |
| Immediate expiry with an impossible clock constraint | final analogue setup fails; compiler exits 1 |

Later expiry can retain an improved completed round, so its bitstream need
not match the initial one. The test checks replay of the returned routing
rather than rejecting a legitimate improvement. The failing-clock test still
has positive hold slack and exits unsuccessfully because setup is negative.
The exact stopping boundary depends on host speed: a round may complete just
before expiry. Results record whether expiry occurred between rounds, during
candidate selection or during rerouting; in-progress cancellation must log
restoration of the best saved routing.

To reproduce with the checked-in menu synthesis fixture:

```sh
python3 mistral/tests/gpurouter/menu_ddr_seed1.py \
  --nextpnr build/nextpnr-mistral --gpu-cpu --output /tmp/menu-budget-base
python3 mistral/tests/analogue_budget.py \
  --nextpnr build/nextpnr-mistral \
  --checkpoint /tmp/menu-budget-base/routed.json --output /tmp/menu-budget-check
```

A separate immediate-expiry replay of the previously hardware-tested
100 MHz SDRAM checkpoint also passes final timing and preserves its exact RBF
SHA-256:
`d65bb881197b03d70495105ac0d59a16a46414785f085f91f19f8d2b664ab641`.
No new hardware run or full SDRAM board timing qualification is claimed.
Commands, inputs, reports and logs were retained locally; their hashes and
results are in
[analogue-budget-reference.json](../../mistral/tests/ramtest-io/analogue-budget-reference.json).

Validation used the CPU reference backend. Cooperative checks surround the
same backend API for device routing, but a GPU device run has not been made.
The earlier seed-4 fresh compilation was not repeated; these checks exercise
the cancellation and restoration paths using routed checkpoints.

### Stop reasons and `analogueMaxRounds`

These cases were run at PR #332 head `8b18750c` on the CPU reference backend.
Each is a fresh full flow of the retained menu DDR synthesis (seed 1,
74.25 MHz). "Unattainable" adds `analogueSlack=1000000` and
`analogueRipSlack=1000000`, so repair continues until a limit stops it.
Final timing passed in every completed case. Fmax is the final analogue
`ddr.scanout.clk` result.

| `--gpu-opt` | `stop_reason` | `rounds` | `reroutes` | `candidate_passes` | `improve_passes` | `time_budget_truncated` | Fmax |
| --- | --- | --- | --- | --- | --- | --- | --- |
| defaults | `target_met` | 1 | 0 | 0 | 4 | false | 99.45 MHz |
| `analogueMaxRounds=1` | `target_met` | 1 | 0 | 0 | 1 | false | 84.18 MHz |
| `analogueMaxRounds=1`, unattainable | `round_cap` | 2 | 0 | 1 | 0 | false | 96.21 MHz |
| `analogueMaxRounds=4`, unattainable | `round_cap` | 5 | 1 | 3 | 0 | false | 106.46 MHz |
| unattainable, no action cap | `reroute_cap` | 9 | 3 | 6 | 0 | false | 106.46 MHz |
| `analogueTimeBudget=0.000001`, unattainable | `time_budget` | 1 | 0 | 0 | 0 | true | 80.16 MHz |
| `analogueTimeBudget=0.000001`, `analogueImproveRounds=0`, `analogueLutPinTrials=1` | `time_budget_post_route` | 1 | 0 | 0 | 0 | true | 80.16 MHz |
| `analogueRounds=0` | `disabled` | 0 | 0 | 0 | 0 | false | 80.16 MHz |

With `analogueMaxRounds=1` and an unattainable target, the first action was
a candidate pass. Its routing was timed again, then the cap stopped repair
before any re-route: `stopping after 1 repair actions (1 candidate passes,
0 reroutes; analogueMaxRounds=1)`. With `analogueMaxRounds=4`, the actions
were two candidate passes, one re-route and one more candidate pass. With the
target met, `analogueMaxRounds=1` allowed one improvement pass:
`stopping improvement passes after 1 repair actions (0 candidate passes,
0 reroutes, 1 improve passes; analogueMaxRounds=1)`. Both expiry cases kept
the initial routing; their RBFs match the disabled run. The defaults and the
unattainable `analogueMaxRounds=1` case were each run twice, with identical
RBF bytes and `analogue_repair` objects.

`analogueMaxRounds=-1` is rejected when repair starts, after the initial
route, and the compiler exits with code 125. The message states that the cap
covers candidate passes, re-routes and improvement passes and that `0`
disables it. A rebuild with that message reproduced the defaults row and the
unattainable `analogueMaxRounds=1` row byte for byte.
`mistral/tests/analogue_budget.cc` checks the rejection message, the
`disabled` stop reason and its `--report` JSON object.

To reproduce a row, add its `--gpu-opt` settings to:

```sh
mkdir -p /tmp/menu-stop
gunzip -c mistral/tests/gpurouter/menu_ddr_seed1/synth.json.gz > /tmp/menu-stop/synth.json
build/nextpnr-mistral --json /tmp/menu-stop/synth.json --device 5CSEBA6U23I7 \
  --qsf mistral/tests/gpurouter/menu_ddr_seed1/constraints.qsf \
  --sdc mistral/tests/gpurouter/menu_ddr_seed1/clocks.sdc --freq 74.25 --seed 1 \
  --router gpu --gpu-cpu --rbf /tmp/menu-stop/core.rbf --compress-rbf \
  --write /tmp/menu-stop/routed.json --report /tmp/menu-stop/timing.json \
  --gpu-opt analogueMaxRounds=1 --gpu-opt analogueSlack=1000000 \
  --gpu-opt analogueRipSlack=1000000
```

### Replay comparisons with repair provenance

`timing.json` now carries per-run `analogue_repair` provenance, so
`mistral/tests/routed_placement_route.py` no longer compares every
`timing.json` byte for byte. The fresh controls (disabled, recorded and
empty-plan) and the full-route group replays still require identical
`timing.json` bytes. Route-preserved replays (preservation, hand-off identity
and preserved group replays) and the optional `--baseline` comparison parse
`timing.json` and require every key except top-level `analogue_repair` to
match. `core.rbf` stays byte-exact in all of them.

A route-preserved replay imports already-repaired data trees at user strength.
Repair can then stop on a different condition, or after fewer passes, with the
same routing, bitstream and timing results. An untouched pre-change
`--baseline` compiler omits the key. The baseline record now reports
`RBF_bytes_exact`, a measured `timing_bytes_exact` and
`timing_without_repair_provenance_exact` instead of
`RBF_and_timing_bytes_exact`. That script needs Yosys and was not re-run for
this record.
