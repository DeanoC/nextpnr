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
