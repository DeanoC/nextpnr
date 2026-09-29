# Local Path Remap Implementation Plan

> Execute inline with superpowers:executing-plans; user authorized implementation.

**Goal:** An opt-in, generic report-guided LUT composition/local-placement experiment with full-route qualification.
**Architecture:** Read a previous legal-route report after fresh placement, validate graph/path identity, enumerate equivalent composed-LUT and whole-LAB candidates, qualify a bounded shortlist with STA, then reroute the selected candidate normally.
**Tech Stack:** nextpnr-mistral C++, json11, GTest, HIP router.
**Spec:** ../specs/2026-09-29-local-path-remap.md

## Global constraints
- Default absent option is identifier-neutral. No hard-coded design names/coordinates/masks.
- Preserve INIT, existing LUTs and all side users; exclude frozen/region/cluster/boundary cells.
- Candidate selection is not a timing-closure claim. Keep the installed pinned binary unchanged.

## Review focus
- Constants/inversions/shared inputs must compose exactly (Task 1).
- Stale report edges/placements must fail before mutation (Task 2).
- Rejected candidates restore graph, aliases, indexed users and BEL state (Task 2).
- FF movement must retain control polarity and pass complete affected-LAB legality (Task 2).
- Final route may regress despite STA; never adopt solely on prediction (Task 3).

## Tasks
- [x] 1. Add failing exhaustive composition tests in `mistral/tests/local_remap.cc`, implement pure composition helper `mistral/local_remap_policy.h`, prove constants/inversions/aliases/input limits.
- [x] 2. Add real backend tests for selection, stale reports, protected cells, movement and rollback; implement `mistral/local_remap.cc`, CLI/Arch hook and focused documentation. Run full backend tests and absent-option control.
- [x] 3. Run current/frozen RTL with bounded ranked candidates; retain raw reports and compare all final clocks/hold. Record outcome, get fresh source review, commit qualified compiler work. Update FES lock only if result warrants adoption.

## Execution ledger
- Base: nextpnr d91c902b, branch feat/mistral-local-path-remap. Installed pinned compiler unchanged.
- Ruling: two fresh runs instead of in-place post-route edits: avoids stale analogue caches and lossy JSON restore; costs one additional placement per candidate.

- Validation: final backend binary 40 tests: 39 passed, one optional external-fixture skip. Eleven focused composition/backend tests pass. Fresh independent source review clean after adding implicit-clock/hard-boundary user checks; observed both regression tests fail before the fix. Current discovery finds twelve candidates with predicted group improvements 1085–1575 ps; full route pending.
- Real-design preflight correction: the conservative hard-consumer guard rejected command-valid input sharing on current RTL. The frozen design instead has seven distinct inputs in its final LUT pair and exceeds the six-input composition limit. Neither strict attempt routed. Admit only modeled, unfrozen hard data consumers with constrained connected clocks and individually nonregressing timed setup slack; keep clock/I/O/unmodeled consumers and translated-FF boundary outputs excluded. Reject the finite INT_MAX untimed sentinel. Positive live-clock, protected-consumer and unrelated-clock regressions pass; fresh source review clean. Full backend now 42 tests: 41 passed, one optional fixture skip. Routed qualification remains pending.
- Current single-group route completed: memory108.365845 vs109.051254 MHz baseline, pixel83.063377, capture464.968536; no final reported hold violations. Critical path moved to the unmodified enable of the neighboring FF in the destination LAB. Reject standalone trial. Extend with explicit --remap-groups 1..8 (default1): examine at most32 additional whole LAB groups, use provisional STA to rank improving groups by original failing slack, retain up to budget-1, then rerun full legality/STA/hold/boundary guards. Require >=250ps improvement for every added endpoint and restore all indexed users on rejection; untouched users retain original indices on acceptance. No RTL names or special endpoint filters. Frozen refined preflight also rejects the seven-input pair; it exits before routing. Current multi-group routing and list-only rollback control are underway.

- Multi-group validation: both added backend tests pass; full backend has 43 passing tests and one optional external-snapshot skip (44 total). Independent source review found no blocker. Current routed qualification remains pending; installed compiler unchanged.

- Final qualification: groups8 candidate0 retains four LAB groups/seven enables; legal full route/RBF and final analogue memory112.170502MHz versus109.051254 baseline, pixel82.453827 versus81.307426, capture464.968536 unchanged. No final reported hold violations. Latest groups8 list-only run restores the complete 14862-cell design byte-for-byte, SHA7008ed7126181998f43222ad387b667b963dc10abb05fe374ddd3fd9a28c97ee. Single-group108.365845 remains rejected; frozen seven-input pair declines before routing. Retain multi-group result as a one-design/seed experimental gain, not a default or130MHz closure. New worst path is a separate burst-count enable cone; stacking further remaps requires additional work. Installed compiler/lock unchanged.
