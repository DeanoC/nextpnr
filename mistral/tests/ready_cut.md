# Ready-enable Boolean cut diagnostic

`NEXTPNR_MISTRAL_READY_CUT=<prefix>` runs after the previous timeout refinement
and enable-copy diagnostics. It is fixture-specific and disabled by default;
an absent or empty prefix returns before allocating identifiers.

The original port1 seven-LUT cone is checked for exact masks, connectivity and
signal pin states. The diagnostic introduces one ALUT6 computing
`from_core = lock_qualified & ~draining & ~skid & ~finishing & (read | write)`.
It changes the original enable root to ALUT5 computing
`(m_read | m_write) & ~ready & (filler | from_core)`, using existing filler.
The original root Q net and all 107 ENA users remain intact. The six shared
interior LUTs, the four-user HPS replica, all FFs, all other cells and their
placements/pin states remain unchanged. Root input loads change, and the direct
HPS ready load deliberately increases by one; the load audit records these.

The new ALUT5 is explicitly tested at the original root BEL after rewiring and
before binding from_core. A deterministic
legal geometric search initializes from_core within radius six of the original
enter_read BEL, using input delay plus delay to the original root anchor.
It then initializes the root within radius six of its original BEL, using all
bound inputs and output sinks. All other cells remain fixed. Protected LABs and
occupied sites are excluded. One legal BEL per LAB represents the coordinate-only
input/ENA predictor; ties use BEL-name order.

A fresh timing graph then refines root followed by from_core for at most two
passes. Every accepted move preserves all 107 current setup slacks and improves
the worst by at least 1 ps. The final result must also preserve every ORIGINAL
endpoint slack and strictly improve its worst. A fresh analysis checks predicted
hold nonregression and every occupied BEL must remain legal. Failure aborts
before routing; no claim of a production rollback is made.

Before/after JSON, complete pin states, protected LABs, initial geometry,
refinement trials/moves, endpoint slacks and load counts accompany the audit.
The optional imported snapshot preflight does not restore the full clock
context. Fresh placement requires the memory clock constraint and repeats all
timing guards. Its routed result, not imported predicted timing, qualifies the
experiment. The independent checker proves the actual packed functions across
all 1,024 arbitrary boundary assignments; no reachable-state assumptions apply.
