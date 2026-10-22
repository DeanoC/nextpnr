# PC/XT rejected-move native clock diagnostics

Compiler `180bacd2b84eca6583268849f04033e99c62c53a` adds diagnostic logging to
`--critical-move-route-budget`. Base compiler is `f956382e`, FES base `cffbea46`.
The acceptance guard, candidate generation, route budget and rollback are
unchanged. Each routable trial reports every native launch-clock setup/hold
margin before and after the move, with delta in nanoseconds; missing/new clock
coverage is reported explicitly. Positive hold margins remain protected.

The frozen binary SHA256 is
`6ae87006e3d13eefaa01ef35fa0e2939c1cb0aef8071713ad388d47fa19b01a0`.
The source/tree identity is in the capture. Two native guard and physical
rollback tests pass. This diagnostic-only change does not add new acceptance
logic. The replay itself checks the log rows against the native baseline and
unchanged final artifacts.

## Same-binary comparison

Both runs use the same placed input, constraints, seed 1, weight 2000 and exponent
5, CPU-reference routing, and 600-second limits. CPU/system and microcode remain
100 MHz; pixel remains 74.25 MHz. The baseline reproduces all four prior artifact
files byte for byte, including routed JSON (no normalization needed).

Eight trials again retain no move: four cannot complete local routing; four
complete routing and regress only CPU/system setup (`clocks[0]`).

| Trial | Moved cell / target | CPU setup before (ns) | After (ns) | Delta (ns) |
| --- | --- | ---: | ---: | ---: |
| 1 | address-next LUT3 / MCOMB.6.36.54 | -1.638 | -2.545 | -0.907 |
| 3 | carry decode LUT4 / COMB.16.35.0 | -1.638 | -2.445 | -0.807 |
| 5 | address-next LUT3 / MCOMB.6.36.55 | -1.638 | -2.546 | -0.908 |
| 7 | carry decode LUT4 / COMB.16.35.1 | -1.638 | -2.457 | -0.819 |

All other setup margins and all hold margins remain exactly unchanged in every
routable trial. There are no missing/new clocks and no clock-margin improvements.
The parser verifies all 32 rows, their deltas and correspondence to the native
baseline. Full cell identities are retained in the route log and connectivity
capture.

Candidate RBF, routed JSON, timing and endpoints equal baseline byte for byte.
Independent comparison checks 12,605 cells, 15,736 nets and 17,199 endpoint rows.
Final WNS remains CPU -1.638 ns, microcode -0.358 ns and pixel -0.228 ns; 343
setup failures and 105 unsupported boundaries remain. The outer retention
checker still rejects incomplete native coverage.

## Interpretation and next step

The rejection is a CPU data-path setup regression, not a positive-hold or another
clock-domain margin tradeoff. Neither moving the address LUT closer to its
neighbors nor the carry-decode moves improve the worst native CPU margin.

The next bounded compiler repair should preserve branches to stationary sinks
on shared input nets and reconnect only the moved sink branches. Continue to
reroute a moved driver's output as required, preserve the full legality and
native timing guards, and test exact rollback before another route comparison.
This reduces the changed routing footprint before trying a coordinated LUT
pair; simply adding more whole-net single-cell trials is not justified.

Connectivity evidence in `incident-connectivity.json` records every logical
neighbor, port, type and BEL for the four tried cells. Some incident inputs have
31 sinks; the microcode-address LUT's shared input has 14. Rebuilding these whole
nets can alter paths to stationary consumers. Clock-margin regression alone does
not prove which branch or endpoint caused it. Keep this distinction explicit
before selecting a coordinated footprint.

## Evidence and accounting

The capture retains frozen inputs, exact commands, compiler identity, build/test
logs, both route receipts/artifacts, independent preservation/endpoint checks,
and the parser and native margin rows. Every member is SHA256-verified by
readback against `manifest.json`; executables are excluded.

Archive SHA256: `8a2738d0e086be6cfdf99d02a1b91120444b313c8643ff932d063e2a58902a3a` (33 verified members).
This comparison charges **457.853168 seconds**, leaving **14005.573293 seconds**
(about 3 h 53 min) of the renewed six-hour route allowance. Earlier route charges
remain intact.

This is CPU-reference host diagnostic evidence only. No hardware or GPU lease
was used. No RTL, clocks, toolchain locks, recipes, seals or shared contracts
changed. Compiler #266 and FES #979 remain draft; timing FES #840 stays open.
Complete routed coverage, the unchanged multi-seed gate, a fresh producer and
exact-artifact hardware acceptance remain required for integration.
