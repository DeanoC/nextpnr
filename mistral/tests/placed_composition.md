# Diagnostic paired relocation/composition probe

This branch is a throwaway experiment for the FES RAM-test timing comparison.
It is not a general optimization pass or a default compiler feature.

Set `NEXTPNR_MISTRAL_PLACED_COMPOSITION` to an existing-directory output prefix
and `NEXTPNR_MISTRAL_PLACED_COMPOSITION_MODE` to `relocate` or `compose`.
The hook runs after the opt-in enable-replication pass. Without a nonempty
prefix it returns without interning identifiers or changing the context,
even when the mode variable is set.

The exact measured port1 original enable driver, producer, replica, BELs,
masks, connections, pin states and user counts are asserted. Both modes
transform the original to ALUT4 temporarily and perform the same site search.
They never move another cell or relax LAB legality. The original LAB is
excluded: its 42 used inputs would become43 after composition. Search is
bounded to Manhattan radius3, excludes occupied memory and protected LABs,
and ranks by distance, maximum predicted input plus maximum predicted output
delay, then BEL name. This geometric score is a heuristic, especially for
HPS pins; it is not a calibrated latency or a timing guarantee.

Both modes move the original to the identical selected legal ALUT4 site.
`relocate` restores the exact original ALUT3 mask, ports, pin data and indexed
input-net user stores before binding there. `compose` absorbs the producer
ALUT2 into the original: A=enter_read, B=cmd_ready, C=cmd_valid, D=enter_write,
mask0x3020. All16 input rows are checked against an independent Boolean
expression. A and Q remain connected throughout. The producer remains for
its other users, and the four-sink enable replica stays unchanged.

Evidence consists of `.before.json`, `.after.json`, a `.pins.tsv` sidecar for
each, and `.experiment.tsv` recording mode, cell, old/new BEL, mask, search
distance and score. Snapshots are evidence, not supported routing restart
files. Every original placement except the selected cell, every other cell's
parameters/connections/pin states, and every original user index are checked
in process. Routed analogue timing and final legality remain necessary.

Tests:

```sh
build/nextpnr-mistral-test --gtest_filter='PlacedCompositionTest.*'
MISTRAL_COMPOSITION_TEST_SNAPSHOT=/absolute/path/replication.after.json \
  build/nextpnr-mistral-test --gtest_filter='PlacedCompositionSnapshotTest.*'
```

The optional snapshot test also requires the adjacent `.pins.tsv` sidecar.
The test-only importer restores the known packed second PLL output, which the
snapshot writer omits when scalar `outclk` and indexed `outclk[1]` coexist.
It performs no packing, placement or routing and does not establish a supported
route-replay path. The synthetic backend fixture
checks both modes, matching destination BELs, all16 truth rows, logical and
physical input mapping, and preservation of the unrelated placed cells.
