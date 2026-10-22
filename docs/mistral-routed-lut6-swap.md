# Experimental routed LUT6 input swaps

`--critical-lut6-swap-budget N` permits at most 16 native trials after a fresh
ordinary full GPU route with RBF output. The default and explicit zero perform
no work and create no setting. Saved JSON settings never enable this pass.
Reloaded, partial, forced-timing, frozen/scaffold and move-repair executions
are rejected. The existing single-net route repair may precede this pass.

Discovery reads actual negative native setup path witnesses, including additional
endpoints in each clock pair. It logs the negative-row and witness counts, the
public report bound, every eligibility rejection, and each two-net trial result.
Candidates are ordinary weak LUT6s with six distinct connected PIN_SIG inputs.
Only transpositions that preserve all 64 mask rows are eligible. Both selected
input nets must have exactly one sink and ordinary unprotected LUT/FF drivers.
Constants, folded inversion, shared fanout, protected routes and mode changes
are outside this first scope.

The trial preserves the LUT type, BEL, mask, output, physical pin maps, all LAB
state and register selectors. It exchanges only the two existing input net/user
slots, refreshes that LUT's cached architecture information, and reroutes those
two nets with the existing bounded local CPU router. Foreign wires remain
obstacles. Failure, rejection and exceptions restore both complete original
trees/strengths, slots, architecture/LAB state and native configuration/cache.
No cells, nets, aliases, route-throughs or timing assumptions are added.

Every output branch must traverse characterized LUT, carry or buffer arcs to
complete native FF/M10K setup and hold captures. Public, unknown, cut, hybrid,
asynchronous-clear, cyclic or over-bound branches reject. The existing private
pair pass also now rejects FF asynchronous clear and incomplete setup/hold rows;
its supported primitive scope is unchanged.

Terminal input delay ranks candidates only. Retention requires fresh skewed
and reference-free native analyses, unchanged complete clock metrics, and the
existing full endpoint/metadata guard: every known setup and hold margin is
nonregressing, untimed extrema and all contracts are preserved, and an already
failing setup row gains at least 20 ps. At most one swap per LUT is retained.
Final JSON materializes the live repaired routes after bitstream construction.

This remains a diagnostic compiler option. Unchanged unsupported coverage does
not become qualification. Production timing and exact-artifact hardware gates
remain separate from a successful native trial or full route.

Upstream source coverage is structural as well as native. Every LUT6 input is
walked through supported LUT/buffer/arithmetic arcs to characterized FF/M10K
launches, with a depth limit of 64 and a work limit of 4096 net/user visits.
Every live source and combinational input must have exactly the structural
clock/edge inventory reported by native setup-window hints. Missing clocks,
undriven or public branches, unsupported/asynchronous origins, cut relationships,
malformed identities and cycles reject. Only explicit native PIN0/PIN1 or exact
MISTRAL_CONST identities count as constants; other launch-free combinational
cones reject. This catches an omitted upstream origin even when another branch
provides valid endpoint timing rows. Full source and downstream coverage is
rechecked before every trial and after actual rerouting, both with and without
clock skew. Existing complete endpoint, clock, setup and hold guards remain.

## Optional fresh second discovery

`--critical-lut6-swap-rounds 2` enables at most one additional discovery round,
requiring a positive swap budget. The default is one round. The existing
`--critical-lut6-swap-budget` remains the total trial cap across both rounds,
with its maximum of 16 unchanged. A second round starts only if the first
retained a swap and trial budget remains.

Each round discovers the current failing endpoints and critical input arcs from
the retained skewed and reference analyses and complete endpoint inventory.
Proposal lists, coverage caches and the per-cell accepted-swap set are rebuilt;
a previously changed cell can therefore be considered on its new critical
input. Every trial still checks current source/capture coverage, exact local
legality and rollback, all setup/hold and clock guards, and the required 20 ps
setup gain. A rejected trial does not advance the retained analysis. There is
no clock reduction, additional timing exemption or enlarged routing budget.

Explicit second-round logs retain each discovery and its trial/retention counts;
the final eligible count sums proposals across the executed rounds. Default
one-round logs and ordering are unchanged. Saved settings do not enable a
second round. A full same-source paired route must establish any actual gain.
