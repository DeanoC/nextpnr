# PC/XT ordinary HeAP parameter matrix

Completed 5/5 requested seed-1 routes. All are diagnostics; no sealed production or hardware acceptance.

| Weight / critexp | Setup failures | System ps | CPU ps | ROM address ps | ROM return ps | Pixel ps | Hold failures | Unsupported |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2000 / 5 reference | 14 | -434 | -173 | +124 | +1257 | +2005 | 0 | 105 |
| 300 / 2 | 537 | -3494 | -3494 | -466 | +1023 | +1449 | 0 | 105 |
| 300 / 5 | 148 | -1387 | -840 | -354 | +1713 | +985 | 0 | 105 |
| 1000 / 2 | 48 | -841 | -537 | -227 | +674 | +2626 | 0 | 105 |
| 1000 / 5 | 91 | -1233 | -1117 | -303 | +1752 | +1551 | 0 | 105 |
| 2000 / 2 | 309 | -1585 | -1577 | -786 | +1428 | +2465 | 0 | 105 |

CPU margins are the CPU subset of same-system-clock destinations. ROM-address and ROM-return cohorts remain separate. Native schema, complete flag, every clock cohort and all Fmax records are retained in `analysis.json`. Each `complete-comparison.json` retains every native endpoint row, before/after margin, timing window/flag and full snapshot metadata; no intersection is used as acceptance.

New full-route wall time, including lease waits: **1813.00336930016049 seconds**. The reference was already charged. Five initial sandbox-denied wrapper launches spent 0.5072072022594512 seconds before nextpnr started: they are retained in `launcher-sandbox-denied/`, with **zero GPU/full-route charge**. There is no overall route cap.

All cases use frozen compiler `a361eac060ebc25f9f70ef3bbc126814069d346b`, binary SHA `e1a121d0de5e496afd96328ed435e77c0b78f913fcfd3768283c01b12313deac`, original raw synthesis SHA `b4c44b2b66faaf8dd1019c6d8de119c55eecb49856158019ac8899a869de028b`, original QSF/SDC, seed 1, 100 MHz, GPU routing, hard-block native windows, corrected spreading and the same proved 17-literal overflow root. Arrival-pin assignment and the new assignment-cost experiment remain disabled. Only the listed HeAP weight and exponent change. The original synthesis is bound to the retained PC/XT pad-pipeline diagnostic; this is not a fresh authenticated producer build.

The two-job gate binds the parent-confirmed completed ROM receipt. At most two matrix processes run concurrently, through `scripts/gpu-run --phase-lease --owner pcxt-timing` and `GPU_LEASE_MAX_PER_OWNER=2`; other owner jobs share the same GPU cap. No vendor/device is pinned. Native router bounds are preserved, with no external timeout or GPU-job termination. `gpu-before.log`, `gpu-after.log` and native device/backend records preserve available contention evidence; wall charges conservatively include all waits.

Exact wrapped/full commands, environments, source/input hashes, start/end UTC, native return codes and final artifact hashes are in each receipt and `runs.jsonl`. The frozen binary, source snapshot and input hashes are checked both before and after each full route. Launch failures stop the controller and cannot be counted as native timing outcomes. No source, clock, lock, seal, shared contract or RTL changed.

The five requested cases are (300,2), (300,5), (1000,2), (1000,5), (2000,2); the existing (2000,5) reference is not rerun. No additional seeds are authorized by this matrix. A zero-setup candidate must be reported for further review before additional seed qualification; unsupported boundaries and hardware acceptance remain independent gates.

All five alternatives regress against the 14-failure reference. **Retain weight 2000 / critexp 5** for subsequent original-source work. None closes setup; no extra seeds were run. All five preserve the full 17199 endpoint identities/windows/flags and all clock/IO/reset/registered/unregistered/physical-obligation contracts. Complete constant-output, static-chain and unsupported-detail records differ with placement/helper choices and are retained verbatim; they are not normalized away or asserted physically equivalent.

This capture does not supply an independent whole-graph function/pin proof across the five changed placements. It retains all final routed graphs and native records for that review. All runs remain incomplete coverage with 105 unsupported boundaries, and no hardware or sealed acceptance is claimed.

The archive contains full original reference and five candidate artifacts, inputs and compiler source bindings. After extraction, `python3 analyze.py --archive-mode --require-complete` and `python3 report.py` repeat the complete native comparison and ledger from included bytes. Archive mode checks the recorded frozen binary identity; the executable is omitted, so this is report replay, not a new tool execution. `manifest.json` binds every captured member; complete readback and offline replay are recorded separately.
