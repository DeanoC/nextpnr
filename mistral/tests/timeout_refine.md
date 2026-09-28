# Two-cell timeout placement diagnostic

`NEXTPNR_MISTRAL_TIMEOUT_REFINE=<prefix>` runs after the placed timeout rewrite
and retained-enable copy. This fixture-specific experiment only moves
`timeout_partition_ch1_control9` and `timeout_partition_ch1_control0`.
Everything else, including both existing enable replicas, remains fixed.
An absent or empty prefix returns before allocating identifiers.

The search performs at most two coordinate-descent passes in that order, within
Manhattan radius six of each cell's original BEL. Protected LABs are excluded;
only free legal slots are considered. The original placement is the no-op
candidate. The first legal BEL by name in each LAB represents that LAB: these
ALUT6 input/output paths have coordinate-only route predictions and no dedicated
LUT-to-FF DATAIN arc. Logical LUT pin delays are independent of the BEL slot.

Each trial re-runs propagated timing with updated predicted route delays. All
29 downstream FF ENA setup slacks must be nonregressing, and the worst must
improve by at least 1 ps. Equal scores use BEL-name order. There are at most
340 candidate timing runs plus four accepted-state refreshes. A fresh final
analysis rejects new or worse predicted hold violations, and all occupied BELs
are checked. Failure aborts before routing. This is a bounded diagnostic, not a
production optimization or a routed-timing guarantee; coordinate descent can
miss improvements requiring simultaneous moves.

The prefix receives before/after JSON and complete pin-state sidecars,
protected LABs, candidate trials, accepted moves, all endpoint slacks and an
audit manifest. Snapshot restoration in the optional backend test includes
carry clusters and pin states but does not reconstruct the complete clock
context. Its timing is diagnostic only. Fresh placement requires the memory
clock constraint and repeats the setup/hold analysis in its original context.
Set `MISTRAL_TIMEOUT_REFINE_TEST_PREFIX` alongside the existing timeout and
retained-enable test prefixes to exercise that imported preflight.
