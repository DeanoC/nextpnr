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
clocking/primitive timing and physical LUT tables. Export requires repair budget
0 (the default); a positive `--critical-cohort-budget` is rejected before packing
so the model describes the unrepaired baseline placement.

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

The original results below were recorded on checkpoint `a95851bf`, before
main's LUT6 select-delay correction (`434b8664`). They are historical results
for that timing model; calibration files must be regenerated after timing model
changes. The main-integration validation is recorded separately below.

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

Additional paired baseline/calibrated runs on seeds 1, 4 and 5 use the
immutable checkpoint binary and the same input hashes. These runs are
sequential; all reported values use the final analogue model.

| Seed | Baseline system MHz | Repaired system MHz | Setup WNS (ns) | Hold WNS (ns) | All timing passes |
| --- | --- | --- | --- | --- | --- |
| 1 | 43.5445 | 43.0700 | -4.070 | +0.724 | No |
| 4 | 46.0024 | 46.9043 | -2.172 | +0.791 | No |
| 5 | 51.6182 | 51.7143 | -0.189 | +0.748 | No |

Seeds 4 and 5 improve but remain below the system constraint. Seed 1
regresses; pixel and audio remain passing on all three seeds. Across all five
tested seeds, only seed 3 closes every final clock/setup/hold constraint.
This supports retaining the disabled-by-default experimental status.

The experiment remains disabled by default. Timing closure is demonstrated for
seed 3; **#171 and #112 remain open**, because seed 2 still fails and hardware
acceptance is unavailable. Routing changes can still regress predictions.

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

Follow-up seed-2 candidate experiments retained all timing guards. Adding
singleton combinational alternatives reduced final system timing to 47.9272 MHz.
Reserving two hop slots for near-worst register controls, including wires under
700 ps, accepted an additional enable-register move but reached only 48.0307 MHz
on seed 2. It improved seed 3 to 52.9773 MHz (+0.272 ns setup), so the outcome
depends on the route. Neither policy was retained. Both rejected seed-2
routes made a VRAM address input the worst endpoint, despite preserving
predicted endpoint timing. Seed 2's tied `idle_addr` bits 7 and 9 share their enable driver in LAB (15,23); bit 7's data driver is
also there, while bit 9's data driver is in LAB (12,15). Moving bit 9 toward
its enable can worsen its data input. A future search should consider both
input cones and verify the resulting complete route.

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
the generated PLL clock constraints.

The passing seed-3 RBF and its matching baseline were each uploaded to the
designated MiSTer Pi through the leased raw-RBF diagnostic endpoint. Both
timed out at the development probe and reported `stopping` with
`reboot_required`; the repaired-image HDMI capture was blank. This does not
establish hardware timing or gameplay acceptance, and the shared baseline
failure prevents attributing it to the repair. The lease client recovered
through its development reboot path after each Stop. Final target health was
ready, runtime idle and lease free. Evidence is in the validation JSON and
`/tmp/sg1000-cohort-171/hardware-seed3`. The checkpoint is available in
[draft PR #181](https://github.com/DeanoC/nextpnr/pull/181).

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

## Integration with main, 2026-10-08

Merged main `6053c560` into this branch. Test registration retains both the
cohort CLI and LUT6 fixture tests. The shared LUT-delay helper uses main's
corrected physical E/F select delays. Virtual and actual FF route-through
selection both honor FES reservations, retaining main's checkpoint restoration,
socket input accounting and bitstream mux corrections.

A fresh paired seed-3 baseline/calibrated run with the unchanged st569 inputs
reaches **49.6401 MHz in both modes**, with **-0.997 ns system setup** and
**+0.730 ns hold**. Pixel and audio pass. No trial survives all guards at
budget 64; the final timing summaries and RBF bytes match the baseline exactly.
The earlier seed-3 closure belongs to the previous timing model and is not
current closure evidence. Regenerate calibration after timing-model changes.

All 54 native tests passed (40 cohort, 11 FF4, two pin-map and one LUT6 timing).
Five CTests passed (cohort CLI, both LUT6 fixtures, placement checkpoint and
physical LUT timing), along with the FES reservation/route-preparation script.
The added reservation regression checks that placement preview and routing
both preserve the FF data connection when its paired LUT is reserved.
Full commands and hashes are in `main_sync_validation` in the validation JSON;
logs and routed artifacts are under `/tmp/sg1000-cohort-171/main-sync-*`.

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
