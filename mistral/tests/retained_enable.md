# Retained enable diagnostic

`NEXTPNR_MISTRAL_RETAINED_ENABLE=<prefix>` enables one fixture-specific copy after
the placed-timeout rewrite. An absent or empty prefix allocates no identifiers.
The selected source is the retained DDR1 enable driver identified by the routed
116.333 MHz control path to `ddr1_test.burst_end_MISTRAL_FF_Q.ENA`.
This is an experiment selected using measured timing, not a generic optimizer.

The original replication pass rejects that entire 106-user net because 28 users
occupy carry-protected LABs. This diagnostic keeps those complete LAB groups on
the original driver. It can move only complete unprotected groups, must include
the measured critical endpoint, and leaves at least one original user. It adds
at most one copy, never copies an existing replica, checks the remaining
configured copy budget, and requires its input nets to be disjoint from those
loaded by the existing HPS replica.

Source/input boundary guards match the ordinary replication restrictions. Each
input's predicted delay must not increase; every moved output must improve by
at least 250 ps. The search is bounded to radius three around the critical FF's
actual LAB. All original BELs, metadata, pin states, and unchanged port user
indices are checked. All occupied BELs must remain legal. Fresh predicted hold
analysis must show no new or worsened violation, or the process aborts before
routing. No in-process rollback contract is provided.

The hook writes `.before.json` / `.after.json` with effective `.pins.tsv`, the
existing replication-checker-compatible `.replicas.tsv`, and `.protected-labs.tsv`,
`.inputs.tsv`, `.outputs.tsv`, `.audit.json`. The audit distinguishes measured
endpoint selection from predicted criticality and reports clock-constraint
presence and estimated hold/setup results.

For the optional actual snapshot preflight, use the existing baseline input and
carry restoration from `placed_timeout.md`, select region `roots`, and set
`MISTRAL_RETAINED_ENABLE_TEST_PREFIX` to a separate prefix. The test runs the
unchanged timeout mapping and then this copy in one context. Snapshot import
omits clock/PLL-phase constraints: its STA results are diagnostic only. The
fresh production hook requires a memory-clock constraint and repeats the hold
guard. Fresh routed timing and the independent snapshot proof remain decisive.
