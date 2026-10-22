# PC/XT native-window hard-block assignment, 2026-10-22

The best seed-1 diagnostic reduces whole-design setup failures from 369 to 14,
with worst setup slack improving from -1602 to -434 ps. All 192 microcode
address paths pass in that combined run, with +124 ps worst slack. Timing
closure is still rejected: setup failures, per-endpoint margin regressions,
incomplete timing coverage and physical qualification obligations remain.
No hardware ran, and no clocks, constraints, keep guards, production locks or
module contracts were changed.

## Frozen implementation and checks

Compiler result `a361eac060ebc25f9f70ef3bbc126814069d346b`, tree
`5f166732030cdc2a298b6930d837a70e1b7af7a6`, contains the default-off
`--placer-heap-hard-block-windows` policy. The exact executable SHA256 is
`e1a121d0de5e496afd96328ed435e77c0b78f913fcfd3768283c01b12313deac`.
Its 14 source files were frozen and built before committing those exact bytes;
the build label therefore retains parent `1a5a69bb`. Source hashes, committed
result identity, source copies, patch, build log and test logs are retained.

The separate heuristic timing accessor preserves native phase, duty, false-path,
multicycle and clock-group semantics without changing the strict exporter.
The planner requires complete known timing relationships on every active RAM
input and output branch. It minimizes a bounded deterministic all-RAM matching
cost, using each incident branch's shortest native window, and rolls back the
complete bucket if native placement legality fails. Protected, constrained,
fixed, unsupported, cyclic, excessively deep or nonseparable direct RAM
interactions reject the proposal. Ordinary spreading, raw-HPWL snapshot
selection and subsequent simulated annealing remain in use.

Validation before the PC/XT comparison: 18 focused native tests and five CTests
pass. An independent exhaustive-permutation oracle checks 1,500 randomized
assignment matrices, including 21 infeasible cases. Native tests cover phase
and duty windows, exceptions, complete domain pairs, unknown/hybrid cones,
protected cells, direct RAM interactions, depth limits, and rollback rejection
at every position of a three-cell transaction.

## Paired CPU preparations

All cases start from identical synthesis, QSF and SDC bytes, seed 1, 100 MHz,
timing weight 2000 and criticality exponent 5. `overflow17` additionally uses
the separately proved four-cell overflow reduction rewrite. Its exhaustive
Boolean proof and an exact comparison to the prior overflow placed graph are
retained; its effect is not attributed to the RAM policy alone.

The default placement's complete modules match the previous compiler. Replacing
only creator metadata restores exact JSON bytes. Explicit setting zero preserves
the complete logical graph and every BEL after a complete two-ID alias
renumbering. Conflicting dedicated and generic settings exit 125 without an
artifact. Every enabled iteration accepts all 318 RAMs against 553 native sites,
covering 7,902 timed input edges, 1,830 output branches and 1,866 static inputs.
There is no assignment fallback in these runs.

`analyze.py` checks all 10,448 original cells, 13,597 exact net aliases and signal
identities, every connection, function, clock, polarity, saved pin state and
attribute. Only declared settings and BEL changes are permitted. Nine deliberate
corruptions reject. The 48 kept address FFs remain WEAK and can move physically;
`keep` preserves the logical FF rather than fixing its BEL.

In the combined policy/corrected-spreader cases, the selected native assignment
places seven ROMs in column 5 and nine in column 14, even though the ordinary
fallback spread trace still places the ROM bucket at column 41. Final annealing
retains columns 5/14. Debug traces retain solved, ordinary spread, accepted
assignment and final positions separately. The policy-only case selects an
eastern solution instead, so neither the cost heuristic nor one placement trace
is a guarantee of timing closure.

## Final routed results

All margins are picoseconds from the final native analogue export. Each route
uses the exact corresponding CPU preparation recipe, removing `--no-route` and
debug logging and adding final report/RBF outputs. Every original cell's BEL
matches that preparation after the full route.

| Seed-1 recipe | Whole setup failures / WNS | ROM addresses: failures / WNS | 90 ROM returns WNS | Pixel WNS | Minimum related hold |
| --- | ---: | ---: | ---: | ---: | ---: |
| Default | 369 / -1602 | 148 / -1406 | +1 | +675 | +795 |
| Windows | 247 / -1307 | 100 / -674 | +489 | +1434 | +787 |
| Windows + corrected spread | 110 / -967 | 3 / -199 | +915 | +2354 | +781 |
| Windows + corrected spread + overflow17 | 14 / -434 | 0 / +124 | +1257 | +2005 | +822 |

All four routes return 1 for timing failure and emit complete diagnostic
artifacts. All have zero related-hold failures. The enabled cases decrease
9,401, 8,953 and 8,994 still-positive endpoint hold margins relative to the
control; these remain recorded regressions. No aggregate Fmax substitutes for
the complete endpoint gates.

The disabled full route reproduces the previous compiler's RBF, timing report
and native endpoint report **byte for byte**. Its routed JSON modules are also
exact; replacing only the creator label restores identical JSON bytes.

## Complete graph and native contract evidence

`route-graph.py` proves each routed artifact against its corresponding proved
placed graph. Every original function, parameter, port, connection, alias,
logical pin state, polarity, attribute and BEL is preserved. Generated helper
counts are respectively 2,157, 1,789, 1,848 and 1,949. Each helper must be an
exact identity BUF reading the original FF DATAIN signal, with one unique
generated alias, one driver and its sole exact FF consumer. Parameters,
PIN_SIG states, binding strength and the FF half are checked. Every generated
cell and alias is accounted for; no inventory intersection or name-only
substitution is used. Complete helper ledgers and physical pin-map differences
are retained in the four `route-graph-*.json.gz` files. Thirteen mutations of
the actual control graph reject.

`route-analysis.py` compares all 17,199 endpoint/clock tuples and every native
window and setup/hold applicability flag. There are 17,194 timed setup and
related-hold rows; five asynchronous relationships remain untimed. All global
clocks, IO, output, registered, unregistered, reset, forwarded-clock contracts,
physical obligations and unsupported inventories are exact. Six endpoint and
contract corruptions reject. All 192 detailed address paths retain their exact
FF launch identity and 2.5 ns native window; path segment sums agree with native
setup margins and are compared to the corresponding placement estimates.

Physical records are validated against their own final routed graph before
comparison. Both constant-output records change physical driver locations in
each enabled case. Static chains change at 9, 4 and 6 of the 1,888 static
endpoints, while 81 of 105 detailed unsupported records change. Only individually
proved identity helpers and explicit physical location/pin-map fields are
normalized for a separate logical-continuity check. Raw physical record hashes,
all final records, complete pin maps and all 4,191 LAB states remain retained.
This normalization does not establish physical equivalence or waive changed
physical obligations.

`complete` remains false, all 105 unsupported identities remain open, and the
native primitive-clock-margin inventory is empty. Graph preservation does not
prove emitted LUT masks, RBF interpretation, electrical routing, primitive
clock margins or hardware acceptance. No native physical reload or board test
was performed as part of this archive.

## Charges and archive

| Run | Charged seconds |
| --- | ---: |
| Default | 236.56779883988202 |
| Windows | 286.8298792280257 |
| Windows + corrected spread | 505.198825111147 |
| Windows + corrected spread + overflow17 | 494.35987655306235 |

These four completed leased routes charge **1522.95637973211707 seconds**.
The prior cumulative total through the density archive is
22721.06404116749706 seconds, giving **24244.02042089961413 seconds** through
these runs. Receipt wall times include lease waits. CPU preparations are
uncharged. Later seed and ADD experiments are excluded; no total route cap or
external job termination was applied.

The compact capture retains all inputs, CPU and routed artifacts, commands,
receipts, logs, source identity, focused test evidence, complete graph ledgers,
analysis scripts and the referenced proof inputs. Executables and build objects
are excluded but hashed in their original receipts. The archive readback checks
every retained file's size and SHA256. Historic absolute paths remain unchanged
in receipts; analysis scripts can be rerun in the retained task worktree.

The next integration step is the separately retained same-recipe seed campaign
and further proof-gated work on the remaining setup paths. No production lock,
frequency, coverage or acceptance predicate is changed by these results.
