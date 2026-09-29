# Experimental wide reduction balancing (Mistral)

`--balance-reduction-root CELL` selects the output cell of an unpacked
conjunction for rewriting before placement. The option is disabled by
default. It names a cell in the current synthesis JSON, not a saved placement or
timing report. A missing or ineligible root is an error. Repeat the option to
rewrite more than one eligible cone, in the order supplied.

The pass accepts only three ordinary unplaced LUTs for exactly eleven distinct
external literals, or four LUTs for exactly sixteen. It checks each LUT truth table,
including pin inversions, and requires every intermediate output to have one
consumer inside the cone. Kept, clustered and region-constrained cells, special
nets and partially constant LUT pins are excluded. The original root output and
all cells remain. The eleven-literal case becomes 6- and 5-input one-minterm
LUTs feeding a 2-input AND. The sixteen-literal case becomes 6-, 6- and 4-input
one-minterm LUTs feeding a 3-input AND. Inputs with bus names
are ordered by numeric index; other names use deterministic lexical order.

Each input keeps its existing slot in the net's consumer list. The rewrite
replaces that slot's cell and port in place, preserving unrelated consumers and
the traversal order used by analytical placement. This matters even when two
serialized netlists describe the same circuit.

The transformation preserves the selected cone's Boolean function. It does not
predict physical timing or automatically choose a root. For timing work, compare
the unmodified and selected routes with the same synthesis input, device,
constraints and seed. Accept a result only after the final route, all required
clock domains and hold checks are examined. Grouping different inputs into the
same LUTs can change placement and worsen timing.

## Eleven-literal route experiment

A frozen RAM-test experiment selected the existing sixteen-literal cone and
an additional eleven-literal cone with repeated options. Exhaustive equivalence
tests passed, including active-low intermediates and inverted pins. The existing
single-root transformed modules were identical between the retained compiler
and this extension. All three compiler test suites passed.

The complete seed-2 GPU route nevertheless regressed memory from 108.944328 to
102.061646 MHz, pixel from 95.229019 to 91.701057 MHz, and capture from
378.582397 to 257.130524 MHz. It generated an RBF with no final reported hold
violations. The new memory limiter was DDR2 state-to-address-enable control,
away from the rewritten DDR0 error cone. More than 8,200 FF placements changed.

This is a rejected timing candidate, not a production selection. The fresh
report contained no failing DATAIN path, so the report-guided hard-IP capture
prototype could not select a source for composition. That follow-up route was
cancelled before placement completed; it provides no routed timing result.
Preserving the accepted placement while qualifying a local cone rewrite is the
next experiment. The result does not change the accepted 108.944328 MHz control
or the separately measured 110.963158 MHz capture-locality tradeoff.
