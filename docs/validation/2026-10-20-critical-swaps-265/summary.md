# Bounded occupied LUT swaps, nextpnr #265

The opt-in compiler pass improves this frozen diagnostic's system timing, but
it is **not a production-qualified PC/XT recipe or a 100 MHz closure**. Routed
endpoint regressions remain a blocker even when aggregate Fmax improves.

Implementation is `9330517fabfbaeaa0e0071caaa27d6e0ffaa8c8c`, based on unmodified
current main `e904cbfb` (full hashes in `manifest.json`). Historical witness
`c2c01bda` is a different baseline and its 90.777 MHz result is not attributed to
this patch. The only open compiler PR found at the start was unrelated #229.

The source branch is `feat/bounded-placement-swaps`, local checkout
`/home/deano/harnesses/pcxt-compiler-repair/nextpnr`. The detached unmodified
comparison checkout is `../parent`. Nothing was pushed, published or merged.
The original FES worktrees, compiler, diagnostic files, toolchain pins and
hardware were not changed.

## Implementation

`--critical-swap-budget 1..64 --critical-cohort-report baseline.json` runs only
with an explicitly supplied unrouted placed checkpoint and `--no-pack --no-place`.
It is off by default and cannot be enabled by checkpoint settings. It extends
`critical_cohort.cc` using the existing transaction, affected-LAB legality,
physical-pin preview, FF route-through prediction and clock-pair endpoint guards.

Discovery prioritizes carry-to-ordinary-LUT report hops, then long report hops.
It considers at most eight sources, Manhattan radius three, 256 geometric probes
per source, 32 legal candidates per source (four per tile), 64 STA evaluations,
and 128 times the requested budget in total geometric/STA attempts. Both sides
of an occupied-site swap are included in connection ranking. All available
endpoint clock-pair rows are checked globally by native placement STA. Fixed,
kept, region-constrained and clustered cells cannot move. Carry/register sites,
functions, net ownership, pin/control states and clock requirements stay fixed.
Rejected and throwing trials restore both bindings and LAB state. Existing LAB
input limits and mixed arithmetic rules are unchanged.

The pass's guard protects **predicted** failing setup margins, passing endpoints,
related hold floors, unrelated path bounds and all clock Fmax. It does not prove
routed nonregression. Requiring final route-level endpoint qualification before
adoption is essential; the results below demonstrate why.

## Fixed inputs and placement evidence

Input comes from the portable FES benchmark at commit
`0a2ad172142ffbb5d48062e0752d5f4a8739c2e7`,
`docs/validation/2026-10-20-pcxt-local-repair`. Its compressed checkpoint/hash,
QSF, SDC, clocks, 100 MHz requirement, seed 1, GPU router, HeAP weight 2000 and
exponent 5 are unchanged. The replay adaptation explicitly records each new
compiler hash instead of bypassing the original helper's old-hash requirement.
The private two-level ADD mux/PIC shortcut/fixed microcode sites/3500 ps ROM
phase remain diagnostic-only inputs.

Final search attempts 5929/8192 geometries, evaluates 64/64 STA candidates and
retains two swaps. Relative to an unmodified-parent reserialization, the entire
10,462-cell checkpoint changes **exactly four BEL fields**, listed in
`placement.json`: result muxes `eu_alu_out_MISTRAL_ALUT5_Q_12` and `_Q_11`, and
their two ordinary LUT occupants. The carry-named occupant is an ALUT6, not a
moved carry primitive. No functional or pin-snapshot field changes.

The current-compiler host legality audit reports exactly the same 26 failures in
unrelated LAB (24,8) before and after. This is not a whole-checkpoint legality
pass. Their cause remains undiagnosed. The affected LABs pass the implementation's
complete occupied-BEL check. Audit commands and failures are in `legality.json`.

## Routed evidence

All four clock requirements and hold checks are retained:

| Clock | Parent MHz | Final candidate MHz | Parent setup ns | Candidate setup ns |
| --- | ---: | ---: | ---: | ---: |
| System, 100 MHz | 89.581650 | 91.726288 | -1.163 | -0.902 |
| Pixel, 74.250069 MHz | 83.745079 | 86.790489 | +1.527 | +1.946 |
| ROM, 100 MHz | 103.767563 | 103.850456 | +0.236 | +0.241 |
| Capture, 100 MHz | 242.248077 | 242.248077 | +2.936 | +2.936 |

System hold remains +0.723 ns; every other clock's hold summary is also
nonregressing. The intermediate `91b9d99a` longest-hop-only trial reaches
88.833618 MHz system, a regression against the same parent.

Expanded **full-route** reports cover 2358 endpoint/clock pairs each, with 2122
common pairs. These are bounded reports, not complete endpoint coverage.
Selected same-clock CPU DATAIN setup margins are:

| Endpoint suffix under `system.machine.cpu.cpu.u_eu_core.` | Parent ns | Candidate ns |
| --- | ---: | ---: |
| `eu_alu_last_result_MISTRAL_FF_Q_11` | -0.226 | +1.370 |
| `eu_register_cx_MISTRAL_FF_Q_2` | -0.612 | +0.526 |
| `eu_register_ax_MISTRAL_FF_Q_1` | -0.432 | +0.426 |
| Historical witness target `eu_alu_last_result_MISTRAL_FF_Q_5` | -1.043 | -0.837 |
| Regressing `eu_register_di_MISTRAL_FF_Q_12` | +0.391 | -0.583 |

Among common pairs, **15 become failing** and 674 lose more than 1 ps of setup
margin. Thus the candidate fails routed endpoint nonregression as well as the
100 MHz setup gate. The successful placement guards must not be represented as
routed per-endpoint protection. All examples above are derived from complete
reported path segment sums, rounded to 1 ps; the raw comparison is retained.

The final numbers and endpoint comparison are recorded in `runs.jsonl` and
`setup-endpoints.json.gz`. Every reported route is terminal, fails setup, retains
100 MHz, and uses no `--timing-allow-fail`. Full commands, binary/source/input
hashes, environment, GPU selection, reports, RBFs and logs are retained under the
artifact root in `manifest.json`.

The intermediate longest-hop-only implementation is retained as a failed
experiment. It moved display-control logic and regressed system timing; it is
not the final implementation.

The expanded no-route reloads reproduce RBF bytes, but change pixel/ROM timing
summaries. They therefore are not interchangeable with the original route's STA
frame. Their files and charged time are retained, with that unexplained timing
limitation. A final paired run requests expanded paths during the full route to
avoid relying on reload timings for endpoint qualification. Both expanded full routes
reproduce the corresponding original RBF bytes and all four clock summaries
exactly. The reload-specific discrepancy remains undiagnosed and is not used
as qualification evidence.

## Tests and reproduction

All **47 final native CriticalCohort tests pass** on the immutable final binary.
The six new native tests exercise actual occupied exchanges, pin/function
preservation, rejection/exception rollback, protected/clustered/region/absolute
constraints, mixed-carry illegality, and a real production search through occupied
sites. The existing cohort tests cover setup/hold/clock guards, multi-user
regressions, calibration, carry clusters and transactional pin preview.
The cohort CLI suite has 12 passing tests, including default-off behavior,
budget bounds and checkpoint/report restrictions.

The existing LUT-pair CLI suite passes 16/17 tests. Its loaded-SA/explicit-HeAP
expectation fails identically on the unmodified parent, whose command-line
setting override semantics allow the explicit HeAP option. It was not changed
as part of #265. An initial CLI batch overlapped a rebuild and failed to execute
the linker output; its result was discarded and the suites rerun against
immutable binary copies.

Validation commands (logs retained beside this document):

```sh
candidate-final-test --gtest_filter='CriticalCohort.*'
python3 mistral/tests/critical_cohort_cli.py /absolute/candidate-final
python3 mistral/tests/lut_pair_placement_cli.py /absolute/candidate-v2
python3 mistral/tests/lut_pair_placement_cli.py /absolute/parent/nextpnr-mistral \
  LutPairPlacementCliTest.test_effective_loaded_sa_placer_cannot_be_overridden_into_this_pass
```

Host evidence check:

```sh
python3 docs/validation/2026-10-20-critical-swaps-265/check.py
```

For a paired replay, supply the existing portable fixture directory and the FES
misteross module root to `replay-local.py`. Use the **same** fixture for both
binaries and a fresh output directory each time. The helper requires a connected
shared GPU lease runner for actual routes, owner `pcxt-agent`, maximum two GPUs,
strict live GPU, no vendor/device pin and a 600-second timeout. Expected setup
failure returns a nonzero status; consult the terminal receipt.

```sh
python3 docs/validation/2026-10-20-critical-swaps-265/replay-local.py \
  --fixture-dir /path/to/fes/docs/validation/2026-10-20-pcxt-local-repair \
  --runner-root /path/to/fes/sources/misteross \
  --binary /path/to/parent/nextpnr-mistral --source /path/to/parent/source \
  --output /new/baseline
python3 docs/validation/2026-10-20-critical-swaps-265/replay-local.py \
  --fixture-dir /path/to/fes/docs/validation/2026-10-20-pcxt-local-repair \
  --runner-root /path/to/fes/sources/misteross \
  --binary /path/to/patched/nextpnr-mistral --source /path/to/patched/source \
  --output /new/candidate --budget 64 --guidance /new/baseline/timing.json
```

## Integration boundary

Parent owns review and external delivery for nextpnr #265. Review/cherry-pick
this local branch as an experimental compiler change only. Do not update FES
production pins or unpark #836 on this evidence. The next technical blocker is
route-aware prediction or a routed acceptance/rollback policy that protects
individual endpoints, including the displaced LUT's consumers. There is no
justification here for relaxing clock/packing rules or a broad seed search.

FES acceptance still requires a same-recipe two-of-three seed gate, fresh producer
BUILD_ID, exact-artifact timing and separately authorized hardware acceptance.
None was attempted. The tested candidate binary is `experiments/bin/candidate-final`; its SHA-256 is
in every candidate route receipt. CMake was configured at the base revision,
so the cached `--version` text still says `e904cbfb` for both executables. The
compiled candidate source content is exactly implementation commit `9330517f`;
use the recorded source and binary hashes, not the cached version label, to
distinguish these binaries. The parent should configure a fresh build when
integrating the commits.

The routing experiment stops here within its 3600-second
budget. Total charged work is **865.764 seconds (14.429 minutes)**, conservatively
including all lease waits and both no-route reloads. Five full routes and two
reloads are terminal; no routing task remains. There were no timeouts. Complete
accounting is in `manifest.json`.
