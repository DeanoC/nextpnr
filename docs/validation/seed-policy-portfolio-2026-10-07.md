# Atari seed × placement-policy screen, 2026-10-07

## Decision and scope

Policy changes affect closure, but this screen does **not** justify a default
change, a trained selector, or live termination. The predeclared rotating
portfolio did not beat the training-selected single policy on this held-out
sample. Report the negative result rather than selecting another scheduler
after seeing held-out labels.

All 51 declared PNR invocations ran, without replacement or additional seeds.
This was host-only CUDA compilation; no FPGA was programmed and no hardware
acceptance is implied. No FES producer, constraint, toolchain lock, firmware,
shared contract, or nextpnr default changed.

## Predeclaration and provenance

See [the pre-result plan](../seed-policy-portfolio-plan.md), committed/pushed
as `e1c81d41003733d998013a5110df69981d4f12c5` before collection. Canonical
declaration SHA-256:
`7328f9c8c403768b4954633837ca20cdcfaf135a0d6d83d15898291d332f647d`.
The mapped Atari fixture, binary, constraints, argv and required clocks are
identified there. The binary source is deliberately the retained
`9643627f59c2e8171164e872357219471b366cd1`, not current main. The collector
base is `da3c7fff5b85c1ab5da1fa704df0a8e7c94e8c1e`.

One RTX 3090, UUID `e553e0819d33727c4728050882d7e7c1`, was used serially.
Every cohort bound the same native runtime environment:
`sha256:5464dab34fe9d4221a210e094f662d131fc12e16b4d1119590a30869737efa1f`.
Criticality exponent alone varied (2/5/8); timing weight stayed 2000. Training
used seeds 17–24, held-out used 25–32, and seed 17 repeated once per policy.

Training selection was persisted at 01:43:06 EEST on October 7, before the
first held-out run manifest at 01:43:09. Its file SHA-256 is
`e84dbb35f22b1d22b47489aff750d270876504b4622cb36e93b7e7da2e98061c`.
These local timestamps corroborate execution order; they are not independent
cryptographic timestamp attestations. The runner freezes selection before
launching held-out runs, and evaluation recomputes it from training only.

Success requires legal routing, the authenticated table-timing gate, and
strictly positive final analogue setup AND hold margins on each of
`system_clock.clocks[0]`, `pixel_clk`, `system_clock.clocks[1]`.
Legal routing alone, a passing Fmax, or exit zero under `--timing-allow-fail`
is not success. Timeouts remain censored, even if a legal route was observed.

## Observed outcomes

| Exponent | Training passes | Training run cost | Held-out passes | Held-out run cost |
| --- | ---: | ---: | ---: | ---: |
| 2 | 3/8 | 3,875.967 s | 5/8 | 2,969.771 s |
| 5 (baseline) | 4/8 | 1,907.492 s | 0/8 | 2,374.779 s |
| 8 | 5/8 | 1,530.416 s | 1/8 | 1,705.330 s |

Training successes per observed second ranked **8, 5, 2**. Exponent 8 was
therefore the frozen single-policy choice. Exponent 2's better held-out count
is a post-hoc diagnostic, not an unbiased replacement winner. There were 20
successes, 27 legal final-analogue failures, and 4 timeouts across all phases.

Held-out paired outcomes below use P = all-clock pass, F = legal analogue
failure, T = timeout; numbers are observed full-run seconds. All held-out
attempts reached a legal route, but the two timeouts lack a final passing label.

| Seed | Exponent 2 | Exponent 5 | Exponent 8 |
| --- | --- | --- | --- |
| 25 | F / 449.955 | F / 205.358 | F / 248.995 |
| 26 | P / 351.557 | F / 222.243 | F / 283.928 |
| 27 | T / 600.183 | F / 241.633 | F / 211.562 |
| 28 | F / 477.998 | F / 280.704 | F / 230.966 |
| 29 | P / 314.305 | F / 281.543 | F / 211.052 |
| 30 | P / 369.039 | T / 600.186 | F / 203.901 |
| 31 | P / 222.938 | F / 259.582 | P / 114.563 |
| 32 | P / 183.795 | F / 283.530 | F / 200.363 |

The five exponent-2 passes have minimum multi-clock margins 0.066, 0.025,
0.052, 0.057 and 0.053 ns respectively. Exponent 8's sole held-out pass has
0.730 ns. All three policies fail or remain censored on seeds 25, 27 and 28.
The portfolio's union covers five seeds; exponent 8 adds no passing seed beyond
exponent 2 on this sample. This does not prove permanent failure of other runs.

## Predeclared offline schedules

Use the same label-only SHA-256 seed permutations for all methods, scheduler
seeds 0–19. The fixed rotation enumerates each policy × seed once, using only
the frozen training ranking. Replay charges full observed run cost and exposes
the final label only at completion. A budget-crossing attempt spends the
remaining budget without exposing its eventual label. No prefixes or future
durations influence candidate order.

| Budget | Baseline successes | Selected-single successes | Rotating-portfolio successes |
| --- | ---: | ---: | ---: |
| 600 s | 0/20 | 6/20 | 0/20 |
| 1,800 s | 0/20 | 20/20 | 14/20 |
| 3,600 s | 0/20 | 20/20 | 20/20 |

At 3,600 seconds, median time to first success is 959.229 s for the selected
single policy versus 1,086.678 s for the portfolio. The baseline exhausts its
eight candidates without success (2,374.779 s). Its time to success is undefined;
do not calculate a speedup ratio against a nonexistent success. At 1,800
seconds, the portfolio's successful-orders-only median is 809.641 s, but that
omits six failed orders and is not evidence that it beats the single policy.

These are offline serial counterfactuals, not observed online races. Twenty
permutations reuse the same eight-seed population; they are not twenty
independent experiments and provide no cross-design confidence claim.

## Repeatability and costs

For seed 17, final timing-report and RBF bytes match exactly between training
and the repeat under **each** exponent. Exponent 5 fails final timing in both
executions; this completed negative result is repeatable too. Durations differ
and are retained as costs, not normalized away. The initial evaluator labeled
that pair inconclusive because it checked only passing completions; the reviewed
evaluator also checks `analogue_timing_failure`. No run or label changed.

Observed training run cost is 7,313.876 s; repeat checks cost 952.549 s;
held-out runs cost 7,049.879 s. Total observed process execution is 15,316.304 s
(4.255 hours). The serial collection envelope, including freezing/preparation,
was approximately 15,344.993 s, from 23:41:03 to 03:56:48 EEST. That envelope
comes from the runner's persisted start and log completion filesystem times,
not a GPU utilization integral. Evaluator/scoring and later write-up are outside
it. Process elapsed time includes fresh placement/routing startup and timeout
cleanup; cohort freezing overhead is separate, not silently credited as saved
compute. No resumable execution is assumed.

A first deployment would also pay the 7,313.876-second training bill (and
952.549 seconds if repeat validation is required), before any held-out-style
search cost. Training alone exceeds every simulated search budget. Thus these
results do not demonstrate an economical first-use selector. Any future
amortization claim needs explicitly declared reuse and new held-out families.

## Retained evidence and validation

Raw artifacts remain outside git under the enclosing FES worktree's
`out/dev/seed-policy-portfolio/evidence`. The reviewed evaluator authenticates
the collector's result/telemetry evidence, declaration-bound binary/input
hashes, argv/clocks/populations, and common backend/runtime identity. Unstarted,
missing, duplicate or excluded candidates cannot become observed failures.
Repeat checks additionally authenticate actual report and RBF bytes.

| Collection | SHA-256 |
| --- | --- |
| Training, exponent 5 | `af1f59365dad3663f13aa05e14a229717d7e7762e65c38d8ba17f6e189595ad7` |
| Training, exponent 2 | `3f56062faa851aa53ad3deaf7998a11a82f940cc8a590857357901f9a88740d1` |
| Training, exponent 8 | `5dadbe723b128fdfdc49cc081aadd2e50ef8484705bac270a7e588861168cffd` |
| Held-out, exponent 2 | `e664099e3db14579efa17fe75af0f5da6c16e1650fb918cc81786caf647f465f` |
| Held-out, exponent 8 | `618b201e723e648c274f84447a81f9b5dfc591f1372b182b5e325dbaa210af20` |
| Held-out, exponent 5 | `b30010288bb45baba3d601d6069847d01dc01caff1b435ada75210b023b9b4dc` |
| Repeat, exponent 8 | `fa51b0dca5ddda39c2570c153731ec09399727474500203629555e8792c28326` |
| Repeat, exponent 5 | `38ca2c967f8de4c0e4cdbb31e7d133017858fcb5acf70d95e771c24ae4c51774` |
| Repeat, exponent 2 | `10215f75bb07dd2922451cc01af092bd22b73dfc568490b700b8e781c56f3033` |

Original evaluation SHA-256:
`28995ea00209b270d1974fd5ae96a172920fc0850c242b48983f95a718fb1cbd`.
Reviewed evaluation SHA-256:
`0a0391088b594f2a30a5488ae25f0a2882a51b5f87c3795bd931eb641d2ea202`.
The original remains intact; `--report` writes a fresh evaluation file.

Focused validation: 18 portfolio tests and the existing 89 seed-racing tests.
The nine-cohort dry-run enumerates exactly 51 invocations. All nine real
collections were authenticated and evaluated without exclusions. The new
tests include train-only selection, failed/restart costs, budget censoring,
candidate enumeration, bound-population mismatches, unstarted candidates,
artifact tampering, and reproducible completed timing failures.

Next research step, if pursued: predeclare a broader multi-family policy
screen and evaluate selection/portfolio economics on independently frozen
families. Do not choose another policy or scheduler using these held-out results
and then present it as the original successful gate.
