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
- [ ] 3. Run current/frozen RTL with bounded ranked candidates; retain raw reports and compare all final clocks/hold. Record outcome, get fresh source review, commit qualified compiler work. Update FES lock only if result warrants adoption.

## Execution ledger
- Base: nextpnr d91c902b, branch feat/mistral-local-path-remap. Installed pinned compiler unchanged.
- Ruling: two fresh runs instead of in-place post-route edits: avoids stale analogue caches and lossy JSON restore; costs one additional placement per candidate.

- Validation: final backend binary 40 tests: 39 passed, one optional external-fixture skip. Eleven focused composition/backend tests pass. Fresh independent source review clean after adding implicit-clock/hard-boundary user checks; observed both regression tests fail before the fix. Current discovery finds twelve candidates with predicted group improvements 1085–1575 ps; full route pending.
