# PC/XT bounded post-route LUT moves

Compiler `4396050653517490010667cfcd12ea01f168dbf8` adds the default-off
`--critical-move-route-budget` experiment (0..16 trials). Compiler base is
`b79bd884`; FES base is `99a8f065`. This is CPU-reference host diagnostic
evidence only. CPU/system and microcode remain 100 MHz; pixel remains 74.25 MHz.

## Transaction and search boundaries

The pass examines at most 32 failing ordinary LUTs and retains at most four
empty-site geometric proposals per cell. Slack-weighted distance only orders
proposals. Registers, carry cells, protected/clustered cells and occupied target
sites are not moved. Each trial reroutes at most seven incident data nets, each
with at most 64 sinks. Inverted signal pins remain in the footprint; only
implicit constant pins are omitted.

All unrelated nets remain bound. A CPU reverse-Dijkstra search has a shared
500,000-node budget per transaction and obeys architecture wire/pip availability
and existing tree parents. It cannot rip up another net. Placement legality is
checked through the existing cohort transaction. Rejection, exhaustion and
exceptions restore placement, complete LAB state, wire/pip strengths and physical
mux/cache state together. Accepted moves still require the complete native
endpoint/contract guard, nonregressing clock Fmax and all setup/related-hold
margins (including positive margins), and a 20 ps gain at a failing setup row.
Unchanged unsupported coverage remains diagnostic, never qualification.

## Validation

Two native tests pass: full endpoint/positive-hold guard coverage, and a physical
inverted-input LUT-chain transaction. The latter checks byte-identical RBF and
routing restoration after rejection, node exhaustion and exceptions, refuses
protected cells and occupied targets, and checks connectivity/polarity and an
unrelated net after a successful move. The CLI boundary test passes its range,
explicit-only, default-off, full-route and exclusive-option cases. All 30
retention/runner tests pass, including candidate-only leased dispatch of the new
mode. The independent graph checker accepts an unchanged graph and rejects a
cell-function mutation.

Two initial test launches raced executable copies and failed with `Text file
busy` before test execution. Their logs are retained in `copy-race/`; all final
checks above use verified frozen copies. Superseded fixture-preview logs are not
part of the final evidence archive. No route attempt was affected.

## Frozen comparison

Baseline and eight-trial candidate use the same frozen binary, placed input,
constraints, seed 1, weight 2000 and exponent 5. Each invocation has a 600-second
limit and is charged at full wall time. CPU-reference mode is explicit; no GPU
lease or physical kit was used.

Eight trials across four eligible cells retain no move: four complete incident
routing but fail the clock setup/hold guard; four cannot route within the bounded
search. All candidate RBF, routed JSON, timing and endpoint bytes equal baseline.
Independent checking verifies 12,605 cells, 15,736 nets and all 17,199 endpoint
rows unchanged. Setup WNS remains CPU -1.638 ns, microcode -0.358 ns and pixel
-0.228 ns; 343 setup rows fail and 105 boundaries remain unsupported. The outer
retention checker rejects incomplete coverage.

The default-off baseline reproduces prior RBF/timing/endpoint bytes. Its routed
graph matches after explicit bit-ID, CPU-backend-setting and embedded creator
label normalization; source-identity.json records the committed source tree and
binary hash rather than relying on that stale build label.

This is a bounded single-LUT move with incident-net rerouting, not simultaneous
placement of an entire cone. Only four of the 32 examined cells have positive
geometric-gain proposals. It does not rule out a coordinated cone repair.

## Evidence and next integration step

`capture.tar.gz` includes inputs, commands, receipts, logs, RBFs/reports,
source/binary identities, tests and independent comparisons. Executables are
excluded. Original absolute input paths map to `inputs/` via `inputs.json`.
Every archive member is SHA256-verified by readback against `manifest.json`.

Archive SHA256: `a256bc1a29201c83d97d40134c8989e3da15a3e7e85554bea651dd3de7e78906` (35 verified members).
The comparison charges 582.528400 seconds, leaving **14463.426462 seconds**
(about 4 h 1 min) of the renewed six-hour route allowance.

Before expanding the search, the next compiler step should record the exact
regressing clock and margin for each completed trial, then use that evidence to
choose a small coordinated placement footprint. Increasing single-cell trial
counts is not justified by this capture.

No RTL, clock, lock, recipe, seal or shared contract changes. Draft nextpnr #266,
FES #979 and timing issue FES #840 remain open. Production integration still
requires a complete final routed comparison, the unchanged multi-seed gate,
a fresh producer and exact-artifact hardware qualification.
