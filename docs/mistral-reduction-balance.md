# Experimental wide reduction balancing (Mistral)

The current supported cone classes, rewrite rules, placed-search bounds and
rollback guarantees are documented in
[the reduction balancing guide](../mistral/reduction_balance.md).
See [control remapping experiments](mistral-control-remapping.md) for the
available options, pass order and current routed measurement.

## Historical eleven-literal route experiment

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

This was a rejected timing candidate. The fresh report contained no failing
DATAIN path, so the report-guided hard-IP capture prototype could not select a
source for composition. That follow-up route was cancelled before placement
completed and provides no routed timing result. These historical measurements
remain separate from the later control-remapping result.
