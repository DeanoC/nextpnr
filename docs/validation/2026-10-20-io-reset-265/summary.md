# Constrained input endpoint coverage, nextpnr #265 / FES #840

The compiler now exports explicitly constrained unregistered input launches
through native timing into registered sinks. This resolves all 16 SDRAM input
paths in a **host-only, synthetic-budget topology diagnostic**, but does not
qualify the real SDRAM interface. The original fixture remains unconstrained
and incomplete. The four PLL/reset ACLR paths remain unsupported.

Implementation is `3cb77ec2` on `feat/bounded-placement-swaps`, based on
`b0f3ea1acd0170f9734bd9009f3f8ac9ee7282b0`, in
`/home/deano/harnesses/pcxt-compiler-repair/nextpnr`. Nothing was pushed,
published, merged or run on hardware. FES files, production pins, all previous
artifacts and the frozen fixture were preserved. Parent owns delivery/review.

## Implementation and evidence boundary

The common endpoint API has an explicit `allow_external_launch` option. Both
native early and late backpointer walks must terminate at a valid register or
an explicit IO-delay startpoint whose source clock/edge and ordered finite
min/max interval match the arrival domain. Unconstrained/asynchronous sources
still fail closed. Existing API callers default to register-only validation.
The Mistral exporter opts in except for FF ACLR, whose existing synchronous
approximation cannot supply external reset recovery/removal timing.

No STA propagation, report arithmetic, routing, architecture delay coefficients,
clock phase, source latency or clock requirements were changed. Native source
segments already carry the explicit min/max budget. Skewed setup/hold margins
retain the physical capture-clock interval, while unskewed native reports
provide path extrema. The tests check distinct source/capture clocks, falling
source edge, PLL-style phase group, exact min/max margin deltas and exact
capture-latency deltas. In the host topology diagnostic, all 17,147 previously
available dynamic rows, all clocks and all 1,888 static proofs remain identical
after enabling the synthetic input constraints.

Schema 3 requires `io_constraints`, including an explicit empty array when no
constraints exist. The native exporter sorts the saved constraints; the driver
validates finite ordered intervals, direction, edge, clock, port and uniqueness,
and requires exact equality before considering route gain. Omitting the field
from either or both complete snapshots rejects. Schema-2 complete inventories
are no longer eligible. Prior evidence files were not rewritten; only the
old coverage checker's synthetic isolated-row test envelopes were updated to
schema 3 with an explicit empty IO contract.

This uses the existing SDC/`timing/io_delays` machinery documented in
`docs/mistral-io-delay.md`, not a parallel SDRAM model. The unregistered GPIO
boundary is routing ingress/egress. Existing analogue intervals are the
backend's slow-corner model, not an independently characterized fast-corner
hold bound. External clock-pad latency, unmodeled pad delay and board effects
must be incorporated with justified data. RAM Tester's
`mistral/tests/ramtest-io/budget.py` and existing GPIO reference profile remain
the applicable starting points when their topology matches; their example
unmeasured board assumptions are not PC/XT signoff values. Private registered
pad endpoint aliases and output captures remain outside this endpoint API.

## Frozen topology diagnostic

The optional native test imports the original placed checkpoint without routing
or bitgen. It first inventories the unchanged graph, then applies the separate
`synthetic-inputs.sdc`. That file declares arbitrary test-only 1..3 ns arrivals,
referenced to **falling `clocks[0]`**, not inferred from the capture clock.
The resulting native rows preserve rising capture `clocks[1]`. Neither snapshot
is marked final analogue; neither can pass the retention driver.

| Inventory | Dynamic pairs | Static proofs | Unsupported |
| --- | ---: | ---: | ---: |
| Original unchanged graph | 17,147 | 1,888 | 20 |
| Synthetic input constraints | 17,163 | 1,888 | 70 |

Exactly the 16 original SDRAM DATAIN endpoints become represented. The four
original ACLR endpoints remain unsupported. Enabling the existing IO timing
classes additionally exposes **66 output/OE endpoints: 48 I and 18 OE**,
including display and SDRAM outputs. These have no output constraints in this
input-only variant. Independently, the endpoint API still requires registered
capture and does not yet export externally constrained output captures, so
merely adding output SDC is insufficient for complete inventory coverage.
No endpoint was suppressed to report an artificial four-only remainder.
`host-summary.json` lists all resolved, retained and newly visible identities;
`pcxt-host-diagnostic.json.gz` retains both complete native inventories.

No new full-design route, reload signoff or Fmax comparison was performed. The
old baseline/candidate evidence still contains 15 newly failing setup pairs,
7,135 setup decreases and 6,181 hold decreases. The existing evidence checker
passes against the unchanged retained artifacts and independently rejects each
of the 15 newly failing rows under the current guard. The historical
89.581650 -> 91.726288 MHz result is not 100 MHz closure and remains unsafe.

## PLL/reset trace and precise remaining work

Read-only FES `cores/fes-pcxt/rtl/fes_pcxt_top.sv:34-41` declares two two-stage
`async_reg` chains, one on the system clock and one on the pixel clock. Both
asynchronously assert from the AND of the two PLL `locked` signals and shift
the inactive state synchronously. The packed graph has a LUT AND cone feeding
all four ACLR pins. Each first stage has constant-one DATAIN (the mapped
polarity), and exactly one Q consumer, its second-stage DATAIN. The second
stages have 241 and 107 consumers respectively. The physical FF cells have no
retained `async_reg` attribute. `reset-topology.json` records the actual
connections, clock identities, source attributes, pin states and consumers.

Recognizing this topology does not turn PLL lock into a registered launch or
create characterized recovery/removal. No static exemption or synchronizer
waiver was added. The focused tests reject both a PLL-lock-fed ACLR and an
explicitly input-constrained ACLR. Ordinary external resets therefore cannot
be legalized using input data budgets.

A future reset contract must survive synthesis/packing and identify the actual
stage chain, clock/edge, reset polarity, inactive constant, first-stage fanout,
second-stage consumers and allowed controls. It must specify architecture-
supported asynchronous assertion, release/metastability assumptions and any
recovery/removal or downstream release checks; the compiler must validate these
invariants and preserve timed stage-to-stage/consumer paths. The current
synchronous ACLR approximation and missing synchronizer contract cannot justify
waiving these paths. Parent owns any required FES RTL/constraint changes.

For SDRAM, next work is to establish evidenced arrival limits at the existing
model boundary using the actual forwarded clock relation and applicable pad/
board data, then extend output endpoint support and account for all 66 newly
visible IO boundaries. These are prerequisites to further guarded proposal
routes, independently of the known candidate's genuine setup regressions.

## Validation and restart

The final CPU-only compiler/test binaries are immutable in
`/home/deano/harnesses/pcxt-io-reset/final-bin`; version is
`nextpnr-0.11.1-564-g3cb77ec2`. `manifest.json` records exact source, input and
binary hashes. All build and test logs, including development runs and one
misspelled CLI-test selector failure, are retained in that output root.

Final checks:

- Native `IoDelayTest.*:CriticalCohort.*`: 84/84 passed (final log retained).
- Routed driver: 17/17 passed, including native safe synthetic gain,
  setup/new-failure/hold regressions, missing/duplicate/malformed constraints,
  min/max/clock/edge/phase mutations, both-sided metadata omission, external
  reset rejection, exceptions and byte-exact retained baseline artifacts.
- Frozen topology diagnostic: passed, no route/bitgen, explicitly non-analogue.
- CLI baseline model/export case: passed for both default and explicit zero
  budgets, exercising two empty CPU routes and schema-3 final export.
- Previous endpoint-coverage evidence checker: passed with exact old artifact
  hashes and all 15 independent genuine setup regression cases.
- Portable checker in this directory validates both host inventories and the
  synthetic native input fixtures, never authorizing hardware or timing closure.

The only new routing is the two empty CLI fixtures, reserved at 60 seconds per
invocation. The successful invocation took 11.360 seconds; the initial selector
failure is also conservatively charged. Total campaign use is **1962.155 /
3600 seconds**, leaving **1637.845 seconds**. No GPU lease or hardware was used.
Native STA tests and host graph import do not route and are excluded. All route
invocations are terminal; no full-design route was warranted.

Reproduce host checks (replace absolute fixture/output paths as needed):

```sh
final-bin/nextpnr-mistral-test --gtest_filter='IoDelayTest.*:CriticalCohort.*'
python3 mistral/tests/routed_swap.py /absolute/final-bin/nextpnr-mistral-test
NEXTPNR_TEST_IO_CHECKPOINT=/path/to/original/placed.json \
NEXTPNR_TEST_IO_SDC=/path/to/synthetic-inputs.sdc \
NEXTPNR_TEST_IO_DIAGNOSTIC=/new/host-diagnostic.json \
  final-bin/nextpnr-mistral-test \
  --gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints
python3 docs/validation/2026-10-20-io-reset-265/check.py
python3 docs/validation/2026-10-20-endpoint-coverage-265/check.py \
  --artifact-root /home/deano/harnesses/pcxt-endpoint-coverage/paired
```

The unchanged 100 MHz requirement, per-endpoint setup/hold nonregression,
coverage/clock guards and minimum gain remain mandatory. FES same-recipe seed
qualification, fresh producer, exact-artifact timing and separately authorized
hardware acceptance remain open. The historical 26 unrelated LAB legality
failures, reload/full-route discrepancy and inherited LUT-pair CLI issue are
unchanged and were not claimed fixed.
