# Fixed FPGA2SDRAM analytical anchor diagnostic

`NEXTPNR_MISTRAL_HPS_FIXED_ANCHOR=<prefix>` locks the unique FPGA2SDRAM
primitive to its unique compatible BEL through HeAP's existing `ioBufTypes`
seed path. It requires exactly one initially unbound, unconstrained,
unclustered primitive and ordinary full-design HeAP placement. An absent or
empty variable leaves default callbacks and identifier allocation unchanged.

The primitive is absent from analytical placement cells and equation rows;
its analytical centroid remains at the physical BEL. Its seed binding is
STRONG. Immediately before SA, only that cell's binding strength is restored
to the baseline WEAK value. STRONG is not categorically excluded from SA;
restoration preserves the original binding-strength contract and avoids
other strength-dependent differences. The SA algorithm and timing prediction
are unchanged. The independent `NEXTPNR_MISTRAL_HPS_PIN_GEOMETRY` option can
add physical pin offsets to this fixed centroid.

`<prefix>.phases.tsv` records seed, row setup, completed initial solves,
solve/spread/legalise, selected best placement before refinement, refinement
entry, and refinement completion. Row state is unknown before row setup and
after SA, which may reuse cell userdata. The `locked` and analytical-location
columns after SA describe the retained HeAP state; the BEL and strength are
checked against the live context. Two sorted all-cell placement files,
`<prefix>.before_refine.bels.tsv` and `.after_refine.bels.tsv`, permit independent
phase comparison. The diagnostic asserts all anchor invariants while running.

Run `nextpnr-mistral-test --gtest_filter='HpsFixedAnchorTest.*'`. The real small
HeAP fixture proves the disabled HPS receives solve rows, anchoring excludes
it, physical offsets compose with anchoring, and WEAK strength is restored
before and after actual SA. Configuration tests cover identifier-neutral
disabling and rejection of prebound, BEL-constrained, clustered, region-bound,
missing and duplicate primitives. Full RAM timing is a separate experiment;
these tests make no timing-improvement claim.
