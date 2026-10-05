# RAM-test held-out seed-racing gate, 2026-10-06

## Scope and decision

This is the independent held-out follow-up to
`seed-racing-atari-prefix-diagnostic-2026-10-05.md`. It evaluates the frozen
`balanced-prefix-v1` policy on the 100 MHz FES RAM tester. The run was host-only;
no FPGA was programmed and this is not hardware acceptance.

The narrow ranked-success gate passes: a ranked candidate eventually succeeds
in 20 of 20 scheduler orders in each repeat stratum, exceeding the predeclared
18-of-20 requirement. That does not justify an online terminator. Fifteen of
sixteen seeds pass, a random full run finds a success far sooner, and honest
restart-charged successive halving costs more than completing the entire random
cohort. Keep `balanced-prefix-v1` available for offline diagnostics, but do not
implement shadow or live termination from this evidence.

## Target selection and frozen inputs

The first predeclared target was FES C64. Two packer preflights, including one
with the recipe's pinned Yosys revision, stopped before routing because the RTL
explicitly instantiates `CFG_ASYNC_READ=1` M10Ks. Cyclone V M10K hardware does
not support asynchronous reads. The target was therefore invalid for this
experiment; using an older packer to accept it would not make the result legal.
No C64 seed outcome or routing telemetry was observed.

Before any RAM-test route, the replacement target and experiment limits were
frozen. RAM-test was chosen because it is independent of Atari, uses legal
synchronous storage, has three required clock domains, and is timing-sensitive.
The mapped fixture contains 8,207 FFs, 6,542 ALUTs, two PLLs, and no M10K or
MLAB cells.

| Input | Identity |
| --- | --- |
| FES source | `31ba384656ba3f5cefecc9ee089561058f8a0918` |
| Yosys | `886afa63953e97407153e9f4aae25fcedb639696` |
| nextpnr source used by the collector binary | `9643627f59c2e8171164e872357219471b366cd1` |
| nextpnr binary | `aa5bf5001100d26171aaab89af5a50205d81a4fc4fb577ddbdd6948397dad0a9` |
| evaluator revision | `43d3fc0963341811856cffd44c45982dbe7e5e36` |
| Mistral | `7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039` |
| mapped netlist | `f6c406ab649f52e7b8f993a184b2a717a35fa55063017ab11f7437ff4aa77686` |
| QSF | `8f97054248c681aca77fc26f55d6e91478164cc7726ec03034b4e459f1ea48d4` |
| SDC | `3ba280ad420fee63a546a7c5cda15fc6c480de58cad398c67af2e62d1e296041` |
| execution backend | CUDA, NVIDIA GeForce RTX 3090 |

Success required a legal route plus strictly positive final analogue setup and
hold margins on all of `ram_clock.clocks[0]`, `display.pixel_clk`, and
`ram_clock.clocks[1]`. Routing legality and table timing remained separate from
this final all-clock gate.

## Pilot and collection

Seed 1 was an excluded pilot. The first and tenth rankable routing iterations
arrived at 199.591 and 209.509 seconds, resolving the predeclared rounded
checkpoints to 200 and 210 seconds. The provisional pixel-domain label was
corrected from `pixel_clk` to the report's `display.pixel_clk` before cohort
collection; the pilot stayed excluded.

The cohort ran seeds 1 through 16 twice, serially, with a 600-second per-run
limit and 21,600-second total limit. All 32 runs produced complete evidence:

| Result | Repeat 1 | Repeat 2 |
| --- | ---: | ---: |
| Complete successes | 15/16 | 15/16 |
| Legal analogue failure | seed 13 | seed 13 |
| Duration, min / median / max | 111.298 / 233.731 / 438.819 s | 111.290 / 233.671 / 421.600 s |

Seed 13 routed legally in both repeats but its memory-clock setup margin was
exactly 0.000 ns. The positive-margin rule therefore classifies it as an
analogue timing failure. Pixel and capture timing passed; the route was not
relabeled as illegal.

Repeatability was exact where expected. For all 16 seeds, repeat 1 and repeat 2
had identical final clock margins and classification, identical prefix metrics
after excluding wall-clock timestamps, identical timing-report hashes, and
identical RBF hashes. Durations varied slightly and were retained as observed
costs.

## Prefix-only replay

The corrected evaluator first proved that each replicate stratum exactly
matched the 16 candidate identities and repeat indices sealed in the cohort
manifest. Because this cohort predates identity-embedded population fields,
the replay supplied the retained original collection manifest explicitly with
`--cohort-manifest`; the evaluator matched its cohort descriptor and
recorded canonical declaration digest
`686bb9b583f47eecc8035961c3e5d4093f9bf35c738a0249838138d965a573f8`.
Each stratum then used checkpoints
200/210 seconds, quotas 8/2, one exploratory survivor, scheduler seeds 0 through
19, and a 9,600-second budget. No repeat competed with another repeat, and no
future observation, final route label, or numeric router seed was a ranking
feature.

| Median or count | Repeat 1 | Repeat 2 |
| --- | ---: | ---: |
| Balanced ranked-success gate | 20/20 | 20/20 |
| Balanced eventual-success recall | 9/15 (60.0%) | 9/15 (60.0%) |
| Congestion-first eventual-success recall | 8/15 (53.3%) | 8/15 (53.3%) |
| Balanced ideal-resumable cost | 3,139.075 s | 3,138.977 s |
| Balanced restart-charged cost | 5,159.075 s | 5,158.977 s |
| Random orders finding a success | 20/20 | 20/20 |
| Random median time to first success | 257.029 s | 256.982 s |
| Random full-cohort cost | 3,844.922 s | 3,800.076 s |

Balanced ranking preserves one more eventual success than congestion-first at
the median, so the separated timing lane has a real but small prefix-ranking
effect on this fixture. It is not a speedup. The restart model charges repeated
placement and routing honestly: its median cost is about 20 times random median
time to first success and about 34--36% more than simply completing all 16
randomly ordered runs. The ideal-resumable model is not an executable promise.

Six successful candidates in each repeat terminate before the 200-second first
checkpoint, and another terminates before the 210-second checkpoint. Those
outcomes are legitimately observable at their timestamps, not leaked future
labels, but they also make this high-success population weak evidence for an
early ranking policy. The ranked-success gate is reported as declared, while
the deployment decision also accounts for prevalence and restart economics.

## Retained artifacts

The authenticated outputs are retained outside the repository. Their SHA-256
digests are:

- experiment plan: `073d9253503263c00f959d04d3057544f09141e8ec4db87903c8655564b14e23`
- collection manifest: `7bb02496686b4826a3599f0019ece373f84f6d003a8946216e90a7fe57f940de`
- collection summary: `a2db7b7d8a9eb748e67a9ac5c49fcbf96049ea585159721d4f1a2d5a8b83734d`
- assembled dataset: `c7e4eecd515c2469944d280eaf0224143ab1feaec72e34c119ed64a751352b0c`
- declaration-bound balanced replay: `c191025788d91f47b251e3a9cfb3ebce6cc2db2004f7f157132efb5da756af86`
- declaration-bound congestion-first replay: `2f615c707c85b25b7b9e7454a24c803d61630b4eaae5fb0d907b7322b55c066b`

This closes the predeclared held-out gate without changing nextpnr's default
behavior and without authorizing online termination.
