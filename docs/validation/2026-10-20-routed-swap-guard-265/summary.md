# Routed retention guard for nextpnr #265

The known unsafe two-swap candidate is rejected. The retained baseline RBF,
routed checkpoint, signoff report and native endpoint inventory are byte-for-byte
unchanged. This is a **guarded no-op**, not a QoR gain or 100 MHz closure.

The implementation continues local branch `feat/bounded-placement-swaps` from
`7e58803aa44dbae9c2ed2b61e8024db5163185a9`, with commits
`f38454094aa58e09f60fef77bb129125beeff8c2` and
`2aec8f7a871e7ab7aef4a202219746b9bf390b0e`. The latter fixes the setting accessor
in the initial commit. No history was rewritten. Nothing was pushed, published,
merged, deployed or run on hardware. FES files, production pins and recipes are
unchanged.

## Mechanism and scope

`mistral/routed_swap.py` is an opt-in compiler driver. It copies one immutable
compiler and the inputs, runs baseline and candidate in independent processes
from the same placed checkpoint, and selects a whole output bundle through an
atomic `retained` symlink. A rejected or throwing candidate cannot mutate the
baseline Context or its emitted placement, route, pin/LAB attributes, RBF or
signoff results. There is no in-process snapshot/restore or checkpoint reload.
All failed candidate artifacts remain available.

`--routed-endpoints` exports all supported native endpoint/clock-pair rows after
full-route analogue signoff, independently of bounded path-report limits. It
also lists every connected registered/IO endpoint the API cannot cover.
Setup/hold use signoff clock skew; untimed path extrema use an unskewed analyser
in the same final routed analogue context. Complete identical coverage, clocks,
phases/groups and timed/related classification are mandatory. Every setup and
related hold margin, including positive margins, must be nonregressing;
unrelated extrema and clock frequencies must also be nonregressing. One setup
margin must gain at least 20 ps. Prediction only proposes moves.

Bare positive-budget full-route invocations now require an explicit
`--critical-swap-routed-trial` acknowledgement. This emits an unqualified
candidate; the driver supplies it only in its isolated candidate directory.
Budget zero and ordinary invocations are unchanged. Saved timing-allow-fail and
forced execution are forbidden for endpoint qualification.

## Paired evidence

Both fresh full routes use compiler source `2aec8f7a`, the same immutable binary,
seed 1, unchanged QSF/SDC, GPU router, HeAP weight 2000 and exponent 5. The fixture
checkpoint SHA-256 is
`733f1cc8ea765bed46dae44dcd744e4432edd75ddedaf232061c0a1d9756b4a6`.
Guidance is the retained original `baseline-run/timing.json`, not a reload.
Each invocation has a 600-second bound, and no timing-allow-fail flag is used.
Both terminate normally with exit 1 because system setup still fails.

The fresh baseline and candidate each exactly reproduce their corresponding
previous expanded full-route RBF and all four clock setup/hold summaries. The
baseline match also supplies default-off artifact compatibility evidence.
No old/new compiler timing values are combined for the retention decision.

| Clock | Baseline MHz | Rejected candidate MHz | Baseline setup ns | Candidate setup ns | Baseline / candidate hold ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| System, 100 MHz | 89.581650 | 91.726288 | -1.163 | -0.902 | +0.723 / +0.723 |
| Pixel, 74.250069 MHz | 83.745079 | 86.790489 | +1.527 | +1.946 | +0.755 / +0.755 |
| ROM, 100 MHz | 103.767563 | 103.850456 | +0.236 | +0.241 | +4.654 / +4.655 |
| Capture, 100 MHz | 242.248077 | 242.248077 | +2.936 | +2.936 | +5.749 / +5.923 |

Both native inventories contain **17,147 supported endpoint/clock pairs** and
**1,908 unsupported connected endpoints**. All 17,147 available keys match, but
that is not complete timing coverage. Their diagnostic comparison finds **15
newly failing setup pairs**, **7,135 decreasing setup margins** and **6,181
decreasing hold margins** (any negative delta, including positive-to-positive
margin changes). Aggregate hold still passes. The known unsafe case is exactly
reproduced; the driver retains the baseline with reason
`coverage failure: incomplete native endpoint coverage`.

Acceptance fails closed on incomplete coverage before considering any gain.
`comparison.json` reports the available-row comparison as diagnostic only; its
intersection never authorizes acceptance. Native rows and unsupported endpoint
lists are retained in the two compressed endpoint files. The bounded legacy
reports remain available in the full artifact directories.

## Validation and artifacts

- 48/48 native `CriticalCohort.*` tests pass on the immutable committed binary.
- 11/11 routed-driver tests pass, consuming native STA fixtures with controlled
  early/late wire delays. They cover prediction acceptance followed by routed
  setup loss, a newly failing endpoint, hold loss, missing/changed coverage and
  clock classification, duplicate/nonfinite rows, exception retention, exact
  baseline bytes, and safe relative acceptance of a complete native fixture.
- 15/15 cohort CLI tests pass, including default-off/checkpoint behavior,
  full-route trial acknowledgement and rejection of saved timing-allow-fail.
- The successful safe-acceptance test uses controlled native timing and an
  artifact fixture. No full GPU candidate was accepted in this campaign.
- The inherited 26 host legality failures in unrelated LAB (24,8) remain
  unresolved. Prior affected-LAB checks passed; this is not a whole-checkpoint
  legality claim. The inherited LUT-pair CLI 16/17 result and identical parent
  loaded-SA/explicit-HeAP failure remain separate; that suite was not rerun.

The fresh build's version is `nextpnr-0.11.1-556-g2aec8f7a`, matching its source.
Compiler SHA-256:
`f95cffa60910293c467294520e21738abf17f72fb511b7e7d2edb372b7e49395`.
Immutable binaries, all build/development logs and paired artifacts are under
`/home/deano/harnesses/pcxt-routed-guard`; the selected bundle is `paired/retained`.
The manifest records input, binary, source and output identities. The initial
build error and failed fixture tests are retained as development evidence, not
counted as passing tests. One initial test launch overlapped the linker and
failed before execution; subsequent runs use immutable copies.

The two routes consumed **318.393 seconds** including all lease
waits: baseline 148.776 seconds and candidate
169.617 seconds. The CLI suites contain empty-design CPU routes, so their entire reported
119.470 and 137.936 second durations are conservatively charged as 120 and 140
seconds (260 seconds total, including interpreter overhead). Adding those and
the prior 865.764 seconds gives
**1444.156 / 3,600 seconds used**, with **2155.844 seconds
remaining**. Both routes are terminal. No further full-design replay, reroll or seed sweep was run.

Recheck the portable decision and optionally exact artifact hashes:

```sh
python3 docs/validation/2026-10-20-routed-swap-guard-265/check.py
python3 docs/validation/2026-10-20-routed-swap-guard-265/check.py \
  --artifact-root /home/deano/harnesses/pcxt-routed-guard/paired
```

## Open gaps

The unsupported endpoints require a sound extension of native coverage before
this design can retain a candidate. Matching unsupported lists do not suffice.
The available rows also show genuine regressions, so filling coverage alone
would not make this candidate safe. No 100 MHz closure or FES acceptance is
claimed; same-recipe seed qualification, fresh producer identity, exact-artifact
qualification and separately authorized hardware acceptance remain external.
The earlier reload/full-route timing discrepancy remains undiagnosed and no
reload timing was used here. Parent owns review and external delivery for #265.
