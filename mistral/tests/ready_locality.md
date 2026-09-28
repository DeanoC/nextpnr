# Ready-locality placement census

This is a fixture-specific, opt-in diagnostic, not a production placement optimizer.
`NEXTPNR_MISTRAL_READY_LOCALITY=<prefix>` runs after the existing placement hooks.
It measures exactly two simultaneous, same-z translations and restores the complete
original placement after each: 387 cells by (0,41), then 655 cells by (0,43).
The generated payload fixes the selected names, types, original coordinates and
research grouping hash. Existing ready-reuse and Boolean-cut diagnostics must be off.

Destination LABs must be empty. Ordinary selected cells may leave donor LABs
containing fixed carry chains; every occupied BEL is checked after the complete
move. Illegal candidates are recorded and restored without timing analysis. No
logic, state, connection, pin polarity or route is changed. Baseline JSON, pin
states, indexed users and cached LAB input counts must match after restoration.

Timing rows are predictions from the existing placement model, including its
logical HPS atom coordinates. All incident signal nets are followed to sequential
or other primitive input boundaries, including indexed HPS ports and siblings of
shared inputs. Moved FF input ports remain visible even when constant-driven.
Ignored, unclocked and untimed domain pairs are explicit; all reported clock Fmax
values are included. Setup and hold regressions are reported rather than used to
retain a relocation. No candidate is retained and no routed Fmax is claimed.

`ReadyLocalityTest` checks atomic placement/restoration, illegal full ALM packing,
exception restoration, occupied destinations and absent/empty identifier neutrality.
The optional existing `PlacedTimeoutTest.ActualSnapshotPreflight` can enable this
stage with `MISTRAL_READY_LOCALITY_TEST_PREFIX`. That test imports the baseline,
restores known pin/carry/PLL details, all three clock periods and the existing
ram PLL shared phase group (0/6538 ps from asserted packed PLL parameters), then executes the two
trials. It remains an imported diagnostic context, not a complete route replay.
