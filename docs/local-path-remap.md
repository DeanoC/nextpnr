# Experimental local path remapping (Mistral)

`--remap-critical previous-timing.json` uses a prior completed route's timing
report to shortlist equivalent local LUT compositions after a fresh HeAP
placement. Without `--remap-candidate`, it lists predicted candidates and restores
the design. `--remap-candidate 0` keeps the first qualified candidate for the normal
router. This experiment is disabled by default and is not a timing-closure guarantee.

Use identical synthesized input, device, constraints, seed and placement options
for both runs. Relevant report cells, ports, routing edges and tile locations must
match the live design; stale relevant paths are errors. The report is a selection
hint, not a design-equivalence certificate or a saved Context. No pin or PLL state
is imported from JSON.

The current search targets failing setup paths ending in an FF enable, whose final
two combinational cells are ordinary LUTs. It composes those LUT truth tables,
including constants, inverted inputs and shared nets, into at most six inputs.
Both original LUTs and their side consumers remain. One whole LAB group of FF
enables receives the composed copy. FF control polarity and initial state stay
unchanged. Candidate variants keep the registers fixed or translate the group by
one adjacent tile with identical z coordinates.

Frozen, kept, region-constrained and clustered cells, occupied memory/arithmetic
LABs, top-level boundary nets and clock nets are excluded. Ordinary internal nets
sourced by hard blocks can supply the copy. Composed inputs may also feed an
unfrozen modeled hard-block data input with a connected constrained clock, provided
that endpoint has a timed setup path and its individual predicted slack does not
worsen. Untimed data, clock and I/O consumers remain excluded; translated FF
outputs retain the stricter ordinary-consumer-only rule. The pass considers at most eight
reported cones, empty LUT sites within a three-tile Manhattan radius, and twelve
ranked tile/translation variants per cone. Ranking is deterministic. Qualification
requires complete occupied-BEL legality, at least 250 ps improvement of the selected
group's worst predicted setup slack, no worsening predicted clock Fmax and no new
or worsening reported predicted hold violations. All estimates use the existing
Mistral timing model.

Full routing may erase or reverse these predictions. Compare final analogue timing
for every required clock, routing completion and hold results before adopting a
candidate. Keep the previous implementation until that qualification succeeds.
An unavailable explicit candidate index applies nothing and aborts before routing.
