# Routed placement and compound refinement validation

The source PR adds a default-off feedback framework and bounded post-route
refinement. It preserves strict signoff and treats incomplete timing coverage
as incomplete. Related investigation: [issue #300](https://github.com/DeanoC/nextpnr/issues/300).
That broader Quartus timing gap remains unresolved.

## Publication-branch validation

The framework and compound code are integrated with main. Independent packing
and remap experiments are excluded. The
[validation summary](mistral-routed-lut-branch-2026-10-27.json) binds each phase
to its actual compiled source and executable hashes:

| Phase | Source | Result |
| --- | --- | --- |
| Broad component checks | `1ee129ef`, based on main `4d63c8df` | 226 unique native cases, no skips; 20 controller and 14 refinement tests; six additional model/cache checks |
| Current-main integration | `6778705e`, merging main `00f22afe` | Compiler/native builds, 11 focused native regressions and four CLI CTest suites pass |
| Current-main fresh routes | Same merged code, untouched `00f22afe` comparison compiler | 18 direct routes plus complete controller rejection/improvement checks pass |

Both current-main builds use Mistral
`5e311528d8a373503f3508b0dc916e876db0a7a8`. The native integration selection
covers the DATAIN cache correction, stale/undeclared placement rejection,
native electrical-preview restoration, complete calibrated intervals, and
the new clock-edge/range modes. CLI checks include all 14 feedback methods,
10 critical-cohort methods and both timing-mode suites. CI runs the feedback
controller/refiner/CLI suites and the cache/parent-identity regressions.

Fresh disabled, recorder and empty-plan runs have byte-identical routed JSON,
RBF and timing reports. Against the separately built untouched main compiler,
RBF and timing bytes are identical. The only routed JSON difference is the
top-level compiler-version creator line. The test verifies each creator
against its actual executable's version output and checks that the executable
hash stays fixed; every other raw JSON byte must match. This does not discard
graph, route, ordering or timing values.

Complete-tree preservation and handoff reproduce all 26 captured trees and
endpoint rows for both passing and failing targets. Group checks preserve
the exact final placements and rows. Every regressing controller candidate
is rejected. The redundant single-endpoint fixture gains **328 ps WNS/TNS**,
from -4757 to -4429 ps, and its second proposal round reconstructs the exact
selected first-round parent. Its original tight timing target still fails;
the test retains that failure rather than overriding it.

## Historical full-design measurement

The `historical_i386_diagnostic` section of the validation summary records the
completed frozen i386 experiment before integration with newer main. Its
producer SHA-256 is
`1fce51cbf4e87e867b34d2e2ebfa91f895012071f7c0cecf9f54277bfa6d674f`.
All 411 recorded source files remained unchanged during execution. This
experimental tree also contained independent packing/remap investigations
excluded from the PR. These numbers qualify that frozen diagnostic execution;
they are not a new measurement of the publication branch.

| Final analogue result | Exact matched parent | Compound candidate |
| --- | ---: | ---: |
| Fmax | 66.6311264 MHz | 66.7958069 MHz |
| WNS | -5008 ps | -4971 ps |
| TNS | -1006232 ps | -1003484 ps |
| Known endpoint/clock-pair rows | 12920 | 12920 |

The original 100 MHz constraint and seed 1 are unchanged. All sixteen branch
alternatives are actually bound; three pass the original timing guard and
trial 1 is selected. Eighteen setup rows improve, none regress, and all 12920
related hold margins remain unchanged. There are no new setup violations,
clock/classification changes or unrelated-path regressions. Native parent,
every route, historical calibration and reservations match before the proposal.

The derived-cache refresh corrects 3613 recognized native BUF-to-FF DATAIN
caches and removes 11797 native legality failures. Eligibility changes from
false to true under the unchanged predicate. Actual RBF, report, full endpoint
inventory, native identity and model remain byte-identical through the
refresh. No capacity limit or legality test is weakened.

Independent physical checks cover all 20259 raw cells, 21274 route records,
actual LUT functions and 21043 register/control fields. Only the declared LUT
binding and seven input/output/branch trees change. All 241 logical sink arcs
on those changed trees retain native electrical observations and identical
sink contracts. Nonrouting configuration changes only at the declared empty
source/target ALMs.

A separate-process replay reproduces the actual RBF, timing report, native
identity and model byte for byte. Every raw graph field and wire/PIP/strength
triple matches after complete actual-alias resolution of serialized wire
numbers. Every endpoint record and nested ordering matches. The endpoint file
itself differs only in the outer order of 20049 static records; all 12920 timed
records and other metadata retain their original order. The summary preserves
this distinction rather than claiming complete file-byte equality.

Strict final signoff still fails 100 MHz. Five unsupported external I/O
records remain explicit. This one-seed result is diagnostic; it does not
authorize production retention, update any core lock or establish hardware
acceptance. Full-design raw artifacts remain retained outside the source PR,
with their digests recorded in the summary.

## Reproducing the checks

Build the compiler and native tests using the repository's normal Mistral and
test submodules. Focused checks are registered with CTest, and the fresh route
control accepts an untouched compiler for default-behavior comparison:

```sh
cmake --build build --target nextpnr-mistral nextpnr-mistral-test
ctest --test-dir build -R 'routed-placement-(controller|refiner|cli)$' --output-on-failure
python3 mistral/tests/routed_placement_route.py \
  --nextpnr build/nextpnr-mistral --baseline /path/to/main/nextpnr-mistral \
  --yosys /path/to/yosys --output build/routed-feedback-control
```

The route test uses the GPU algorithm's CPU reference, which is route-equivalent
to the device backends under the repository policy. These CPU-only tests do not
access a GPU or require a GPU lease. Device-backed full routes use `gpu-run`.
The test checks default,
recorder and empty-plan neutrality, complete-tree preservation, failing-target
restoration and a measured synthetic proposal. None of those controls supplies
missing external IO observations or physical timing qualification.
