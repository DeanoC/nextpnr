# Experimental critical-cohort placement repair

`--critical-cohort-budget N` enables a post-place Mistral HeAP experiment for
long hops on failing setup paths. `N` limits complete placement timing trials
to 0..64; the default is **0**. It requires fresh, ordinary full-design HeAP
placement. Saved JSON settings cannot enable it. Passing designs remain unmoved.

The source cohort is a cell and its direct same-LAB data neighbors, expanded to
include every member of their clusters. This includes carry members in adjacent
LABs, without automatically including unrelated cells in the source LAB. A
candidate translates the cohort while preserving its ALM arrangement.

Trials first evacuate only occupied destination ALMs. If shared LAB resources
prevent a legal arrangement, they retry with the movable occupants of the full
destination LAB. Displaced units stay at home when possible, then try their own
LAB, nearby sites, source vacancies and exchanged positions. Complete carry
clusters remain intact. Protected occupants remain fixed; a collision with one
rejects the trial. Fixed cells, user regions, frozen FES cells and unclustered
arithmetic cannot move.

The search covers both ends of up to eight critical hops. It ranks closer
destinations using the entire cohort boundary and visits them in rounds so one
hop cannot consume the whole timing budget. Work is limited to 128 participants,
32 destinations per cohort, 10,000 placement nodes per candidate (shared between
both eviction attempts), 100,000 nodes overall, and `min(128, 8*N)` geometry
attempts. These are work limits rather than a wall-clock deadline. An obstructing
third-party cell remains fixed.

Placement STA uses ideal clocks, retaining clock constraints, phases and related
hold analysis. Distance-based clock-route predictions can otherwise dominate
placement selection despite only a few picoseconds of routed skew. Each
STA evaluation, hop selection and destination ranking uses a reversible preview
of routing's physical LUT-input assignment. The preview calls the same pin-map
helper as `reassign_alm_inputs`, including shared inputs, LUT6 and carry modes;
it restores pin maps and LAB state on completion or exceptions. Endpoint timing
rows are captured while those maps are active. FF route-through selection and
physical LUT timing share routing's helpers. STA folds the predicted incoming
wire, buffer early/late logic delay, and local wire into `DATAIN`, without
creating cells or nets. Frozen LABs keep their existing timing and pin maps.

Every candidate must leave all participants placed, preserve relative constraints,
and pass legality checks for every occupied BEL in affected LABs. Fresh STA
must improve worst setup slack by at least 20 ps, preserve every clock's
constraint and achieved frequency, preserve available clocked endpoint timing
rows, avoid new or worse related hold failures, and keep passing setup endpoints
passing. Failing setup endpoints and untimed clock-pair path bounds cannot
regress. Rejection, exceptions and exhausted searches restore BEL bindings,
strengths and LAB input counts. The pass changes placement, not the netlist.

`--critical-cohort-report /path/to/baseline-timing.json` optionally selects hops
from failing same-clock paths in a prior **final analogue** report. Clock
constraints and endpoints must match; routing arcs must match current cell ports
and net drivers. The exact generated `FF$ROUTETHRU` buffer pattern resolves to
the original FF data input. Report coordinates are ignored. With guidance, the selected
endpoints must gain at least 20 ps while the predicted worst setup slack remains
nonregressing. The other timing guards still apply. This requires a positive
budget and fresh synthesis-to-placement input.

`--critical-cohort-model-out /path/to/model.json` optionally writes an enriched
report after a fresh ordinary full route with `--rbf`. The model records every
eligible data arc's measured early/late wire delay, including register buffer
input and local wires. Pairs follow native STA: IO-delay mode uses the routed
quad's early/late envelope; otherwise both entries use the maximum wire delay.
It also records the normalized logical graph, parameters,
port states, device/LAB models, clock periods/phases, timing settings, native
clocking/primitive timing and physical LUT tables. This export can use budget 0.

Pass that file to `--critical-cohort-report` on a fresh placement with the same
seed and inputs. Calibration requires matching data-arc source/sink BELs,
net/driver ports, graph and timing identity, complete unique arc coverage and
finite ordered delays. A mismatch fails before any move. The model loads once
at the initial placement; subsequent trials use measured baseline wire delay
plus the change in the native geometry prediction, clamped at zero. Virtual
register buffers follow the current placement. A newly dedicated LUT-to-register
connection has zero programmable wire delay; the previous route's detour is
dropped. The baseline's existing dedicated connections retain their measured
values. This remains a prediction of changed geometry and congestion.

Calibrated searches select current critical arcs after each accepted move. They
also consider up to eight direct LUT/register packing candidates, alternate
slots and singleton alternatives for unclustered critical registers. Other
cohorts preserve their arrangement. A register candidate can improve its selected
critical input by at least 20 ps while global worst setup stays nonregressing;
all clock, endpoint, passing-setup and related-hold guards still apply. This
allows tied failing inputs to improve individually. Ordinary uncalibrated
searches retain the neighborhood candidates and original register slots.

Placement acceptance predicts route delays. It does **not** guarantee routed
timing or final analogue signoff. Final routing is still required.

## Issue #171 validation, 2026-10-08

The retained st569 SG1000 synthesis JSON was tested on `5CSEBA6U23I7`, with
HeAP timing weight 2000, criticality exponent 5, and unchanged RTL/netlist inputs.
The branch starts at `e35e0088`; the older #112 investigation starts at
`6cb41766` and has different placement/timing results. Each row below compares
repair against its matching current-branch baseline.

| Seed | Repair mode (budget 64) | Kept cohorts | Final analogue system MHz |
| --- | --- | --- | --- |
| 2 | Disabled | 0 | 48.4590 |
| 2 | Physical pins and register route-throughs | 0 | 48.4590 |
| 2 | Ordinary routed guidance | 2 | 47.3664 |
| 2 | Calibrated current arcs and focused registers | 4 | 48.6523 |
| 3 | Disabled | 0 | 49.8504 |
| 3 | Physical pins and register route-throughs | 5 | 50.9243 |
| 3 | Ordinary routed guidance | 2 | 50.5638 |
| 3 | Calibrated current arcs and focused registers | 7 | **52.6343** |

Calibrated repair improves both seeds' system timing. Seed 3 passes its
52.224773 MHz constraint, with **+0.149 ns setup** and **+0.764 ns hold** margins.
Pixel and audio also pass. Seed 2 improves but still fails system setup
(-1.406 ns); its hold, pixel and audio constraints pass. Calibration loads
26,674 measured data arcs per seed. Existing dedicated connections all measured
zero wire delay (340/320 ordinary LUT/FF connections, plus 514/512 generated
buffer-to-FF local connections, for seeds 2/3). A fresh baseline-and-repair
harness run exactly reproduces seed 3's final clock frequencies, setup/hold
summaries, accepted moves and search counters. Baseline RBF files are byte
identical before and after calibration export on both seeds.

The experiment remains disabled by default. Timing closure is demonstrated for
seed 3; **#171 and #112 remain open**, because seed 2 still fails and no hardware
execution was performed. Routing changes can still regress predictions.

The earlier physical-pin-only model, before register route-through prediction,
kept three seed-3 cohorts and reached 51.2164 MHz. Modeling the buffers accepted
two further moves at budget 64 and reached 50.9243 MHz. Enabling arbitrary
register slots without calibration reached 50.7666 MHz (unguided) and 48.9668 MHz
(guided) on seed 3, so alternate slots are now confined to calibrated searches.
Full-LAB evacuation and uncalibrated singleton alternatives were also explored
and were not retained as the default candidate strategy.

Calibration with static 16-path report selection accepted no moves on either
seed. Switching to current calibrated critical arcs accepted two moves per seed
but routed at 47.9800/49.7067 MHz. Adding direct data packing with the global
worst-gain guard reached 48.2416/49.2296 MHz. Seed 3's worst endpoint is its
register **enable** input, not DATAIN. Focused critical-register alternatives
allow tied failing inputs to improve individually while retaining the strict
per-endpoint guards, producing the passing seed-3 result above. Exact commands
and the earlier experiments remain in the validation record.

A guard allowing already-failing endpoints to trade slack within their clock
pair's old worst margin was also evaluated. It achieved 48.4614/50.8001 MHz
without guidance and 47.3664/51.3795 MHz with guidance, for seeds 2/3 respectively.
It accepted more moves but reduced unguided seed-3 timing, so the stricter
per-endpoint guard was retained. A placement-time improvement still need not
produce a routed improvement.

Using the improved seed-3 route as guidance for a fresh placement achieved
49.8008 MHz. Report guidance selects different hops; it does not resume that
report's placement or accumulate its accepted moves.

Routes used the HIP backend on a Radeon RX 7900 XTX, with fresh synthesis-to-RBF
runs and the original QSF/SDC. Baseline and enabled runs shared the GPU in pairs.
`--timing-allow-fail` allowed collection of failed final reports; zero process
exit status is not timing acceptance. Initial placed-checkpoint route replays
were discarded: without IO-delay settings, those checkpoints do not preserve
the generated PLL clock constraints. No hardware execution was performed.

Validation passed 39 cohort backend tests, 13 existing LAB/register/pin-map
tests, eight CLI tests, and the placed-checkpoint and physical-LUT timing
regressions. The cohort tests cover occupied seventeen-cell exchanges, carry
clusters spanning LABs, protected occupants, rollback on rejection/exceptions/
search exhaustion, passing-design preservation, and native setup, hold and
clock-constraint guards, matching routed guidance and rejection of mismatched
clock constraints before any move. Physical-pin tests compare the preview with
the routing helper, measure shared-input delay changes, and verify restoration
after exceptions. Route-through tests compare the folded model with the actually
inserted graph and cover FF4 priority, occupied LUTs, carry/LUT6 and frozen LABs.
Calibration tests use distinct measured early/late wires, compare native
expanded-graph setup/hold results, reject phase/netlist/placement/arc-coverage
mismatches, check zero-clamping and buffer removal with complete rollback, and
exercise direct packing and improving one of two tied enable endpoints through
the production repair loop. Additional native timing tests reject worsening a
failing endpoint or making a passing endpoint fail even when the clock's worst setup
slack improves.

Input hashes, exact routed commands and final clock reports are retained in
[`validation/mistral-critical-cohort-171.json`](validation/mistral-critical-cohort-171.json).
The local logs and full artifacts are under `/tmp/sg1000-cohort-171`.

## Reproduction

Use one unchanged synthesis JSON for both budgets; do not rebuild RTL between
seeds, because SG1000's baked BUILD_ID can perturb the netlist.

```sh
python3 mistral/tests/critical_cohort_qor.py \
  --nextpnr /path/to/nextpnr-mistral \
  --json /path/to/st569/synth.json \
  --qsf /path/to/constraints-oss.qsf --sdc /path/to/clocks-oss.sdc \
  --seeds 2 3 --budget 64 --route --output /tmp/cohort-qor
```

Add `--guided` to use each seed's fresh baseline route as guidance for its enabled
run, or `--calibrated` to export and import its complete measured wire model.
The passing seed-3 result uses `--calibrated`. Omit `--route` for placement-only
comparisons. The harness runs sequentially,
records commands, input hashes, process status, timing status and final reports,
requires every final clock's setup and hold margins as well as Fmax to pass,
and applies a per-run timeout (default 1800 seconds).
