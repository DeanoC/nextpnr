# SDRAM placement reliability after issue #135

Issue #135 is resolved by merged PR #153. This follow-up tests placement
variability at 100 MHz; it does not extend the closure claim to complete board
timing or 130 MHz. The original failing synthesis, QSF and thirteen empirical
native address constraints are unchanged in every run. All placements and
routes are generated afresh using the GPU router's CPU reference backend.

## Placement study

Mistral sets the HeAP criticality exponent to **7**, overriding the generic
CLI default of 2. The historical FES producer explicitly requested weight 10
and exponent 2. `normal_build.py` now passes and records these settings
explicitly, retaining the architecture defaults unless overridden.

Every initial run requests 500 ps of internal setup margin, five analogue
repair rounds and a 1,200-second compiler limit. The target is separate from
the ordinary zero-slack timing gate. All complete results below use final
analogue timing; incomplete runs have no final slack.

| HeAP weight / exponent | Seed | Memory setup slack | Result |
| --- | --- | --- | --- |
| 10 / 7 | 1 | unavailable | timeout |
| 10 / 7 | 2 | +435 ps | timing passes; target unmet |
| 10 / 7 | 3 | -862 ps | timing fails |
| 30 / 2 | 1 | unavailable | timeout |
| 30 / 2 | 2 | unavailable | timeout |
| 30 / 2 | 3 | +252 ps | timing passes; target unmet |
| 10 / 2 | 1 | -69 ps | timing fails |
| 10 / 2 | 2 | +585 ps | timing and target pass |
| 10 / 2 | 3 | +507 ps | timing and target pass |

Weight 30 and exponent 2 change two parameters relative to the baseline.
The 10 / 2 controls isolate the exponent change. Their results favor exponent
2 on this fixture, but do not establish a reliable architecture-wide default.
Paired seeds 4 and 5 were declared before their results, using both exponents
with weight 10 and the same limits. Independent designs remain necessary.

After seed 1 failed at 10 / 2, a targeted run raised the analogue candidate
fanout limit from 64 to 256. It still finished at **-69 ps**, with +420 ps hold
slack. The longer 1,800-second limit did not affect this comparison: both runs
finished before the original limit. A high-fanout net on the worst path is
therefore not sufficient evidence to increase this default. Host load differs
between runs, so elapsed times are not a speed comparison.

No architecture defaults change in this follow-up. Full commands, compiler
and input hashes, complete receipts and timeout records are retained in
[placement-reference.json](../../mistral/tests/ramtest-io/placement-reference.json).
The baseline predates structured timeout receipts; its original timeout
record is retained without assigning a final timing result.

## Selected-artifact hardware comparison

The 10 / 2 seed 3 artifact was loaded on Kit B, followed by the unchanged
original failing package, then the same candidate again. Each load ran for
200 seconds. Package and embedded build identities were checked throughout;
loads were volatile, boot and health remained unchanged, and the test ended
with the core stopped and the lease released.

Both candidate sweeps completed all six SDRAM patterns with zero errors.
All three HPS DDR ports completed seven patterns with zero errors. Candidate
RBF SHA-256 is
`d65bb881197b03d70495105ac0d59a16a46414785f085f91f19f8d2b664ab641`.
This is an experimental post-route package with the historical build ID,
not a newly sealed production FES build.

The original control clearly displays **FAIL** at 100 MHz after all six
patterns, while HPS DDR passes. Its displayed total is `09E79992`, but its
six pattern counts sum to `09E79991`. Consequently this is a
**PASS 0 → qualitative FAIL → PASS 0** comparison, not a qualified exact
scalar error-count comparison. The strict decoder rejects that discrepancy;
`--inspect` retains raw rows and glyph errors without qualifying a result.

The ASUS capture requires `--pixel-threshold 60` to decode the red FAIL text
exactly. The same threshold is used for every candidate and control capture;
the default remains 100 for the earlier capture device. Critical glyphs must
still match exactly. The unchanged final captures are
[first pass](../../mistral/tests/ramtest-io/placement-pass-first.png),
[failing control](../../mistral/tests/ramtest-io/placement-control-fail.png) and
[repeat pass](../../mistral/tests/ramtest-io/placement-pass-repeat.png).
The reference includes exact observations at 120 and 200 seconds and their
screenshot, font and decoder hashes.

## Remaining board coverage

`constraint_coverage.py` inventories saved IO-delay declarations in a final
checkpoint. The hardware-tested checkpoint has **53 active SDRAM directions,
13 declared and 40 missing**. Bidirectional DQ contributes both directions.
Two structurally constant combinational DQM outputs are exempt; registered
constants are not exempt because their clock/reset behavior is unproven.

The inventory exits nonzero for missing declarations. Even a complete
inventory does not prove that paths are timed correctly: clock exceptions,
pad and load qualification, chip/board setup and hold bounds, forwarded
clock pulses, DQ turnaround and capture/consumption cycles require separate
checks. Completing these checks and validating 130 MHz remain later work.
The historical empirical 6.5 ns native address target is not a chip-pin budget.
