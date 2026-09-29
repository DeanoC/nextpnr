# Experimental wide reduction balancing (Mistral)

`--balance-reduction-root CELL` selects the output cell of an unpacked,
four-LUT conjunction for rewriting before placement. The option is disabled by
default. It names a cell in the current synthesis JSON, not a saved placement or
timing report. A missing or ineligible root is an error.

The pass accepts only four ordinary unplaced LUTs whose output is a conjunction
of exactly sixteen distinct external literals. It checks each LUT truth table,
including pin inversions, and requires every intermediate output to have one
consumer inside the cone. Kept, clustered and region-constrained cells, special
nets and partially constant LUT pins are excluded. The original root output and
all four cells remain. Three intermediate cells become 6-, 6- and 4-input
one-minterm LUTs, and the root becomes their 3-input AND. Inputs with bus names
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
same three LUTs can change placement and worsen timing.
