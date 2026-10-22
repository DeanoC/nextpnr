# Experimental live ordinary-LUT input permutation

`--critical-lut-permute-budget N` (0..32, default 0) enables a separate bounded
post-route diagnostic for ordinary ALUT2..5 cells. It requires a fresh full GPU
route with `--rbf`, without reloaded, partial, frozen, CRAM, forced-timing or move
execution. Explicit single-net route repair, LUT6 swaps and private-pair repair
may precede it. Saved settings never enable it. Existing LUT6 and unrouted
private-pair admission boundaries are unchanged.

Discovery uses actual negative native setup paths, capped at 16384 paths per
clock pair and 64 cell witnesses. Each proposal exchanges a witnessed logical
input with another input and transposes the complete LUT table. Current native
terminal-arc delay ranks proposals, with deterministic cell/pin ordering and
round-robin cell groups. Ranking is not candidate timing. A cell is retained at
most once, and at most thirty-two trials run. Increasing the explicit budget
continues the same deterministic proposal order; every trial retains the same
coverage, physical-state and timing acceptance checks.

The transaction keeps the BEL fixed and changes only one LUT's input bindings,
input inversion states, table and native input allocation. All 4..32 raw input
assignments are checked with actual input polarity. Distinct live sources and
all existing indexed user slots are retained, including free-list holes. It
adds no cells, nets, aliases, helpers, control assignments or placement changes.
Native assignment may change other target input maps; all input nets are
snapshotted and their actual sink sets determine the complete routing footprint.

Every occupied half of the affected ALM must contain an ordinary ALUT2..5 cell
in a LAB or MLAB tile. An empty mate half is allowed when no FF depends on it. LUTRAM, LUT6, carry, protected and constrained occupants reject.
Cohabiting FFs require their own local COMBOUT DATAIN and no SDATA. The entire
LAB's native legality, input counts, FF controls and selectors remain exact.
Stationary mates retain exact pin maps and physical half masks. Every configured
field in the affected LAB except the target half's mask remains unchanged.
An allocation that rotates a mate's shared inputs is rejected.

Every changed net retains its original source root and complete paths to all
stationary physical users, including their routing strengths. Shared branches
needed by a stationary user remain bound. The existing bounded local router
extends these trees without foreign rip-up, sharing at most 500000 node visits
across all changed input nets in deterministic name order. Nets are bounded to
4096 routed wires and 64 users. Rejection and exceptions restore logical ports,
parameters, exact indexed stores, native cell/LAB state, trees and configuration
and timing caches.

Before each trial and after actual local routing, both native timing frames must
prove complete structural source coverage through supported LUT/BUF/arithmetic
arcs to characterized FF/M10K launches. Every structural launch must match the
native relationship inventory; omitted origins, missing clocks, public inputs,
asynchronous/cut paths, cycles and bounds reject. Separate ALUT2..5 and LUT6
wrappers share this checked walker without widening either target scope.
All input-net branches, including stationary users, and the target output also
need complete native FF/M10K setup/hold capture coverage.

Fresh skewed and reference analyses and the existing whole-design guard retain
every endpoint/domain/window and coverage contract. All known setup and hold
margins, including positive margins, must not regress, and at least one existing
setup failure must gain 20 ps. Clock metric deltas and rejection reasons are
logged. Final checkpoint serialization refreshes the actual retained state.
Default and explicit-zero execution do not discover, mutate or refresh for this
feature. Structural proof and an accepted local trial are not whole-board,
external-IO or hardware qualification; unchanged unsupported rows stay unsupported.

`--critical-lut-permute-retain-source-exits` is a separate default-false option
requiring a positive explicit permutation budget. For changed input nets whose
ordinary combinational driver is in the protected target LAB, it retains only
original directly rooted COMBOUT-to-output-mux exit wires/PIPs at their original
strength. Native source/exit identity, ownership and NLUT configuration must
match. No new or unrelated resource is reserved. This prevents pruning a retired
branch exit from resetting a protected output selector. The exact configuration
comparison and every setup/hold/clock/coverage acceptance check remain unchanged.
The retained exit can add load relative to pruning; a full native timing rejection
remains possible. Saved settings do not enable this option.
