# Experimental report-guided LUT swaps

`--critical-swap-budget N --critical-cohort-report baseline.json` enables a
bounded placement-only repair on an unrouted placed checkpoint. Use
`--no-pack --no-place`; `N` is 0..64 complete STA trials, default 0. Saved JSON
settings cannot enable it. The report must be a final analogue timing report
with matching clocks and graph hops, using the existing cohort report parser.
Do not combine with cohort placement repair or frozen FES slot/scaffold modes.

The pass extends the critical-cohort transaction and timing guards with direct
occupied-site exchanges. It considers ordinary unclustered LUTs on both ends of
the longest reported critical hops (prioritizing carry-to-LUT hops), including result muxes driven by carry chains. Carry
cells, registers, constraints, functions, nets, port states and physical pin maps
remain unchanged. Fixed, kept, region-constrained and clustered LUTs are excluded.
An occupied destination must itself be movable. Every occupied BEL in both
affected LABs must pass existing legality checks. No input-budget or mixed-mode
packing rule is relaxed. Rejected and throwing trials restore bindings, strengths
and LAB state.

Each iteration considers at most eight LUTs, sites within Manhattan radius three
of the opposite hop end, and 32 legal ranked sites per LUT (at most four per tile), checking at most
256 geometries per source before STA. Ranking sums every incoming
and outgoing data connection of both exchange participants with the existing
critical-cohort weighting. Trials visit different LUTs in rounds. Across the
whole invocation, geometry attempts are capped at 128 times the STA budget.
Accepted moves trigger fresh timing and proposals; exhaustion stops the pass.

Qualification uses native ideal-clock placement STA with routing's reversible
physical-pin and FF route-through preview. One endpoint on a reported failing clock must
gain at least 20 ps, while worst slack and every clock Fmax remain nonregressing.
The existing cohort guard protects all available endpoint clock-pair rows:
failing setup margins cannot worsen, passing endpoints must stay passing,
unrelated early/late path extrema cannot regress, and related hold violations
cannot be introduced or worsened. This is predicted timing only. It does not
promise that every positive setup or hold margin increases, or that complete
routing will preserve the gain. Baseline and candidate must still be compared
with full final analogue setup/hold evidence before adoption.

This is an opt-in experiment for nextpnr #265, not a PC/XT timing-closure or
hardware-acceptance claim. It never changes clocks, pipelines or timing gates.
