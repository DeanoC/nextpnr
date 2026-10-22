# Native timing and fanout weighted hard-block assignment, 2026-10-22

The new opt-in is rejected for the working PC/XT recipe. Its full route has
**295 setup failures, WNS −1655 ps**, versus the frozen reference's **14,
−434 ps**. All five previously failing display endpoints also worsen. The
implementation remains default-off; no production recipe, RTL, clocks,
coverage gate, toolchain lock or hardware state was changed by this experiment.

Source parent is `ffbc638c38fc04e1414ab082a3c57e002983a61f`; implementation commit
is `2fc1b49a25c6ab267fbc88c9d496d2382e9d1062`, tree
`42f47d953dbcad6262b7118fba925ee5f65c25e1`. Frozen binary SHA-256 is
`3ba9e9244b687d64bfa830e09c7361d7e73b269da6a458bf7ee1c2de90d9189c`.
Source copies, hashes, patch, build/test logs and immutable execution receipts
are included. The cached CMake creator label is not the authoritative source
identity. `source-verification.json` compares every frozen source file with the
result commit; the binary was built and tested from those bytes before commit.

## Bounded general change

`--placer-heap-hard-block-timing` requires the existing hard-block-window
assignment option and HeAP. It retains each validated whole native setup-window
weight, divides by actual net fanout, and multiplies by ordinary HeAP's
`1 + timingWeight * pow(native criticality, criticalityExponent)`. Fractional
weights survive until the full site cost is rounded. The complete-cone/window
eligibility, deterministic all-RAM matching, capacity/constraint protections,
native placement legality and all-or-nothing binding rollback remain in force.
Nonfinite/invalid weights and configuration, zero fanout, unsupported windows
and bounded-cost overflow reject the entire bucket. No core names or forced
sites are involved. Missing dependency and dedicated-option/`--set` conflicts
reject before output. Absent/off does not intern the new setting key.

Five pure tests, nine native tests, the CLI suite and nine CPU graph corruption
checks pass. Native rollback tests exercise every post-bind rejection with
both the old windows policy and the new option. Six CPU preparations include
default, legacy windows, candidate, explicit zero, missing dependency and
conflict. Default and legacy complete checkpoints match the previous compiler
exactly after creator-only byte replacement. Raw bytes differ by that label.
Explicit zero preserves the exact graph and placement with only its recorded
setting and numeric-ID bijection. Candidate graph, complete alias inventory,
functions, logical polarities, clocks and keep attributes are preserved.
All candidate iterations accept the complete 318-RAM bucket without fallback.

The 10-iteration ledger verifies all 9,732 edges per iteration. A compiled C++
oracle equals all 97,320 logged native weights exactly. The earlier Python pow
replay differed by at most one float ULP; its matching is unchanged after the
exact correction. These are objective/placement checks, not timing acceptance.

## Final routed result

Each routed artifact has its own exact same-binary, same-input, same-command
CPU preparation. The candidate and old reference use their separately recorded
different compiler binaries; this is not a same-binary pair of full routes.
Old/default placement invariance is established by the CPU checks above.

| Endpoint cohort | Reference failures / worst ps | Candidate failures / worst ps |
| --- | ---: | ---: |
| All native setup | 14 / −434 | 295 / −1655 |
| CPU / system same-clock | 14 / −434 | 260 / −1655 |
| 192 microcode addresses | 0 / +124 | 33 / −1261 |
| 90 microcode returns | 0 / +1257 | 2 / −408 |
| Pixel-clock domain | 0 / +2005 | 0 / +1619 |
| Related hold | 0 / +822 | 0 / +778 |

Failure classification by endpoint/worst-witness hierarchy is separately:
CPU 7→262, CGA 3→6, MDA 2→4, PIC 2→0, other 0→23. These categories are not
clock-domain cohorts. The five original display endpoint margins change
−26→−530, −434→−1302, −369→−1222, −375→−516, and −376→−1194 ps.
The worst VRAM launch sources change, so retained path comparisons describe
changed worst witnesses, not same-source delay measurements. `route-paths.json`
retains exact segment sums, cells, BELs and all 14 original endpoint deltas.

`prove-route.py` verifies every original cell, net alias, function, logical
polarity, connection and placement against its exact preparation. Individually
qualified identity route-throughs account for all generated topology, including
unused aliases: 10,448 original cells and 13,597 original aliases; reference
1,949 helpers, candidate 1,952. No intersection-only comparison is used.

All **17,199** endpoint/clock identities, windows, setup/hold flags and complete
contracts are preserved. All 17,194 timed setup and related-hold margins are
compared, including **8,718 positive hold decreases**. Physical metadata is
validated against each full routed graph: all 1,888 static sink identities and
105 unsupported identities/reasons remain, while changed constant locations,
11 static route-through chains and 81 unsupported physical records are retained
explicitly. Thirteen graph and six endpoint corruption cases reject.

This is a final native diagnostic, not a seal. `complete=false`, 105 unsupported
entries, physical obligations and empty `primitive_clocks` remain. Native
primitive-clock-margin preservation therefore is not proved. Physical LUT-mask,
RBF correctness and board behavior have not been independently qualified. No
hardware ran, and no failed or unknown gate is waived.

The new route is charged **271.9062757040374 seconds**; the reference was already
charged. This receipt is not a new campaign subtotal. The route returned 1
because timing failed; analysis passes mean the retained evidence is consistent,
not that the candidate is accepted.

## Reproduction and capture

Run `analyze.py`, `weight-ledger.py`, `exact-native-replay.py`, `prove-route.py`
and `route-paths.py` against the retained historical paths. The exact oracle
must be rebuilt from its retained source and header if its excluded binary is
unavailable; all ELF binaries are hashed in the capture manifest, not bundled.
Do not rerun `prepare.py` or `route.py` to overwrite immutable captures.

`capture.tar.gz` includes complete paired preparation/route artifacts, RBFs,
source/test evidence, analysis/proof scripts and required earlier audit inputs.
`capture-readback.json` verifies every member's size and SHA-256 after archive
creation. Baseline artifacts and receipts remain immutable. Parent owns the
review and evidence commit; the next timing experiment uses the frozen
14-failure recipe with existing guarded route repair.
