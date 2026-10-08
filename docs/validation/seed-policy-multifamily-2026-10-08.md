# Cross-family policy screen: completed results, 2026-10-08

## Decision

The bounded screen completed, but does **not** demonstrate a general-purpose,
economical policy selector or portfolio. At the predeclared primary 1800-second
budget and hypothetical ten-use training allocation, both proposed methods cost
more than the retained baseline on every held-out family. Keep default policies
and normal full-run execution unchanged; no live termination follows from this
result. There is no hardware acceptance claim.

This is one combined tooling/results delivery after PR #164, not a series of
new baseline-tuning PRs. The earlier negative Atari screen remains valid.

## Experiment and completion

The [pre-result plan](../seed-policy-multifamily-plan.md) was committed and
pushed as `dbe4f48b` before any new route. Training families were Atari/Catch,
seeds 33–36; held-out families RAM-test/Pong/legal synchronous C64 used seeds
33–38. All families ran exponents 2/5/7/8, preserving their original timing
weights and per-family baseline exponents. The same retained CUDA binary from
source `9643627f` was used throughout, not a newly built current-main binary.
Exact source/input/clock identities and limits are in the plan.

All **104** declared invocations across 20 cohorts ran once, serially on the
local RTX 3090, with 600-second per-run limits. Outcomes: 90 all-clock successes,
10 legal final analogue timing failures, 4 timeouts. No candidate was excluded,
replaced, rerun, or silently inferred as a failure. The 127 focused tests pass
(17 multi-family, 21 portfolio, 89 foundation). A fresh authenticated evaluation
reproduced the retained evaluation bytes exactly.

Legality is distinct from final analogue setup AND hold on every declared
required clock and the authenticated table gate. Timeout status stays censored,
even if the trace reached legal routing. Native stdout, telemetry, reports,
bitstreams and their original evidence remain retained outside git.

## Training-only selection

Training scores are the equal-family mean of successes/process-time rates,
normalized by each family's best rate. A fast family cannot dominate solely
because its runs take fewer seconds. All-failure families contribute zero.

| Exponent | Atari passes / cost | Catch passes / cost | Normalized score |
| --- | --- | --- | ---: |
| 2 | 3/4 / 1310.477 s | 4/4 / 36.843 s | 0.874024 |
| 5 | 2/4 / 912.118 s | 4/4 / 36.495 s | 0.862771 |
| 7 | 1/4 / 841.489 s | 4/4 / 37.150 s | 0.687801 |
| 8 | 2/4 / 661.779 s | 4/4 / 36.717 s | 0.996981 |

The ranking **8, 2, 5, 7** was frozen before held-out collection and preserved
across reboot. It was recomputed unchanged during recovery and final validation.
No held-out label, elapsed cost, prefix, or ordinal numeric seed entered this
selection. Historical baseline results on these designs were already known:
this is a fresh-combination, family-separated test, not pristine-design discovery.

## Held-out observations

Each cell is all-clock successes / six seeds, followed by total process cost.

| Family | Exponent 2 | Exponent 5 | Exponent 7 | Exponent 8 |
| --- | --- | --- | --- | --- |
| RAM-test (baseline 2) | 6/6 / 1403.383 s | 6/6 / 1044.721 s | 6/6 / 1010.149 s | 6/6 / 1186.890 s |
| Pong (baseline 7) | 6/6 / 69.270 s | 6/6 / 68.694 s | 6/6 / 68.415 s | 6/6 / 68.616 s |
| C64 (baseline 5) | 3/6 / 2236.417 s | 6/6 / 1316.493 s | 4/6 / 2134.105 s | 5/6 / 1460.931 s |

C64 exponent 2 timed out on seeds 33, 35, 36; exponent 7 timed out on seed 33
and failed analogue timing on seed 37; exponent 8 failed analogue timing on
seed 36. Baseline exponent 5 passed all six. This is evidence against replacing
that retained baseline with the training-selected exponent on this sample,
not an instruction to change FES production configuration.

## Predeclared serial schedules and training economics

Every method uses the same 20 label-only seed permutations, budgets
600/1800/3600 seconds, and fully charged observed fresh-run process times.
Final labels remain unavailable when a candidate crosses its budget. No live
race was executed and no checkpoint/resume savings are assumed.

At the primary **1800-second** search budget, all 20 permutations succeed for
baseline, selected-single and rotating-portfolio on each held-out family. Their
median marginal first-success costs are:

| Family | Baseline | Selected single (8) | Rotating portfolio |
| --- | ---: | ---: | ---: |
| RAM-test | 230.227 s | 143.239 s | 143.239 s |
| Pong | 11.196 s | 11.732 s | 11.732 s |
| C64 | 216.896 s | 286.766 s | 286.766 s |

Training cost is **3873.069 s**. Allocating one tenth of it adds 387.307 seconds
to each proposed search, paid before any candidate label can become available:

| Family | Baseline (no training bill) | Selected / portfolio, ten-use allocation |
| --- | ---: | ---: |
| RAM-test | 230.227 s | 530.546 s |
| Pong | 11.196 s | 399.038 s |
| C64 | 216.896 s | 674.073 s |

Both proposals remain 20/20 successful at this budget but are costlier than
baseline on all three families. First-use training alone exceeds all three
declared budgets, so proposed methods have zero training-charged successes at
600, 1800 and 3600 seconds, without leaking candidate labels. The baseline
continues to pass 20/20.

At the hypothetical 100-use allocation (38.731 s), the 1800-second median costs
are 181.969 s for RAM-test, 50.462 s for Pong, and 325.497 s for C64. Only RAM-test
shows a descriptive gain versus its baseline in that scenario. None of those
100 reuses occurred; do not promote it to an observed amortized-deployment claim.

At the secondary 600-second **marginal** budget, C64 selected-single succeeds
19/20, portfolio 20/20, baseline 20/20. The portfolio rescues one selected-policy
order, not a baseline deficit, and has a higher median cost than baseline.
With the ten-use training allocation, RAM-test proposals succeed 15/20 and
C64 proposals 7/20; Pong remains 20/20. Baselines pass 20/20 throughout.
Successful-orders-only medians do not erase these failed orders. At 3600 seconds,
marginal/ten-/hundred-use comparisons give the same eventual first-success
medians as 1800; no favorable budget was selected retrospectively.

Six seeds and three held-out families are descriptive. Twenty permutations
reuse the same observed candidates, not twenty independent experimental
populations or evidence of broad cross-design generalization.

## VM interruption and kernel strata

The VM restart was requested by the user. Collection paused after 50 complete
attempts, before RAM-test exponent 5; the admission marker was archived without
deleting evidence. No active PNR was killed and no completed route was rerun.
See the [explicitly approved amendment](seed-policy-multifamily-kernel-amendment-2026-10-07.md).

Kernel release changed `6.8.0-136-generic` to `6.8.0-142-generic`; all other
recorded runtime manifest fields and GPU identity matched. Only those exact
runtime-ID/kernel pairs are allowed, with binary/library/CPU/backend changes
still rejected. The evaluator records the amendment and each policy's stratum.

Training and RAM-test exponents 2/7/8 are pre-reboot; RAM-test exponent 5 is
post-reboot. Its family-wide policy-cost comparisons are therefore mixed and
descriptive. The particular RAM-test baseline-2 versus selected-8 marginal
comparison uses pre-reboot runs on both sides; comparisons involving exponent 5
cross kernels. Pong and C64 are entirely post-reboot. No label equivalence is
proved by a minor version number, nor do we attribute observed policy effects
to the kernel without a controlled repeat. No extra control runs were added.

## Costs and retained evidence

Observed process cost: **15941.155 s** (4.428 hours), including 3873.069 seconds
training and 12068.086 seconds held-out. The elapsed campaign envelope is
18219.212 seconds, from 08:45:14 to 13:48:54 EEST on October 7; it includes
operator pause/reboot downtime, cohort freezing, orchestration and automatic
evaluation, not just process execution. That difference is not routing work
or saved compute. Later independent validation and this write-up are outside
the envelope. No GPU utilization integral or measured concurrent makespan is
claimed.

Evidence root: enclosing FES `out/dev/seed-policy-multifamily/evidence`.
The original collection log, resume log, pause marker, frozen selection,
declarations, amendment and all 20 authenticated summaries are preserved.
Digest conventions distinguish canonical JSON (no newline) from file bytes:

| Artifact | SHA-256 |
| --- | --- |
| Canonical pre-result plan | `a33edab985c2b6395e8119b7e641e148010d6da39675588804bfe5219f710ca2` |
| Plan file | `31e3a9348323808f249e6fb137a14e45c47bde4781fa11b5ebea39277c05fe61` |
| Frozen selection file | `a91ab732596b6fcb83db0cf39a241a089384913effd639c0e80b222f79496220` |
| Kernel amendment file | `f191d9072f1d8b8b2fc92b9e509cffbe209621fcfb4d39e9912a31cbb2bb543e` |
| Final evaluation file | `03dd513c29680f0b55572ca37a31548399d4913f1e6c47ab23e5ed4615170772` |
| Completion marker | `11071ca86ec68df040d1c764256fb8fd7ed0618cc7334ce42ded9cd8b0379e49` |

Re-evaluation accepts `--report NEW_FILE` to preserve the immutable original.
All 104 attempts were reauthenticated, with no exclusions and a byte-identical
evaluation. This delivery closes the declared screen. Further research would
need a new hypothesis and bounded protocol; the current data does not warrant
another broad sweep, a default change or a live terminator by itself.
