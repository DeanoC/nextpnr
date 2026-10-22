# PC/XT route-preserving timing trials

The default-off `--critical-route-budget` repair is implemented in nextpnr
`5d35f171a83866162ca0ffafa7818636e287011d`, with rejection diagnostics in
`0c5904669641974339de61c29a0509973e37892e`. FES base is `7ea109f7`.
This is host timing diagnostic evidence, not timing closure or hardware acceptance.

The pass runs after ordinary routing and analogue repair in the same invocation.
It proposes alternative trees for ordinary failing data nets, keeping all other
nets bound. Each proposal must preserve legality, cell/connectivity/placement,
full native endpoint setup and related-hold margins (including positive margins),
clock margins, primitive-clock contracts and coverage identity. It must improve
an already failing setup endpoint by at least 20 ps. Rejection restores the
original wire/pip bindings and physical mux/cache state. Incomplete coverage can
be searched diagnostically but cannot pass the outer artifact-retention gate.
The existing GPU phase lease is released before CPU-only timing trials.

## Complete leased comparison

Baseline and candidate use the same frozen placement, constraints, seed 1,
weight 2000, criticality exponent 5 and binary. The candidate adds only the
8-trial repair option. Each route has a 600-second active limit; GPU lease waits
pause that limit. Receipts record live GPU grants. The driver conservatively
charges full wall time, including waits.

All eight proposals are rejected. All 17,199 matching endpoint rows have
identical setup/hold values. Independent graph comparison confirms all 12,605
cells and 15,736 nets, with no changed route or other graph content after
bijective bit renumbering. The baseline reproduces all four prior baseline
artifact bytes. The driver retains that baseline bundle.

| Clock | Final setup WNS (ns), baseline and candidate |
| --- | ---: |
| CPU/system 100 MHz | -1.638 |
| Microcode 100 MHz | -0.358 |
| Memory 100 MHz | +2.854 |
| Pixel 74.25 MHz | -0.228 |

Both inventories retain 343 failing setup rows, zero failing related-hold rows
and 105 unsupported boundaries. Coverage is incomplete. The CPU worst path
contains approximately 8.014 ns routing and 3.045 ns logic delay; aggregate Fmax
is not an acceptance substitute. No timing or coverage gate is relaxed.

## CPU diagnostics and tests

An eight-trial CPU-reference replay also rejects all proposals. Its RBF, timing
and endpoint reports are byte-identical to the leased baseline; routed graph
content matches after bijective bit renumbering and CPU-backend normalization.
A separate 32-budget CPU replay times out at 600 seconds after 17 rejected
trials. It has no final artifacts and is not a completed 32-trial result.

The implementation passes 62 focused native tests, 18 CLI tests and 29 retention
and runner tests. The rejection-diagnostic follow-up passes both native guard
and route rollback tests. The independent preservation checker also rejects a
mutated cell parameter. Frozen source identities and binary hashes are included.

The diagnostic eight-trial replay completes: every trial is rejected for a
CPU/system clock setup regression. No trial reaches the later endpoint/hold
acceptance checks. Its RBF, timing and endpoint reports are byte-identical to the
leased baseline, and its routed graph passes the same normalization comparison.
Thus these proposals do not supply a timing gain, independently of the known
coverage gap. This does not prove that local repair is impossible. The next
useful investigation is candidate generation against native analogue delay,
including the load and delay of every branch; increasing this search budget
alone is not justified by these results.

## Reproduction and integration boundary

`capture.tar.gz` includes frozen inputs, commands, receipts, raw logs, final
reports and RBFs, failed captures, checkers and test output. Executables are
excluded; source identities and SHA256 hashes are retained. GPU status dumps
containing unrelated owners' commands are excluded; own grant records remain.
`manifest.json` hashes every archive member, verified by readback. `budget.json`
charges every new route, including the failed CPU attempt and diagnostic replay,
against the renewed six-hour allowance, while preserving prior incident history.

The renewed ledger charges **5332.881248 seconds** and leaves
**16267.118752 seconds** (about 4 h 31 min). Archive SHA256:
`b027aa5717877e9b8c215c9dc02ef785badb2da8ae2427a8f05f4c0499c0cb62`.

No kit was claimed or programmed. No RTL, clock, lock, recipe, seal or shared
contract changed. Draft nextpnr #266, FES #979 and timing issue FES #840 remain
open. Before production integration, a candidate must pass the complete final
routed comparison, the unchanged multi-seed gate, a fresh producer, and exact
artifact hardware qualification.
