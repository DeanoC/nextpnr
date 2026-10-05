# Seed-racing Milestone E Atari ST gate, 2026-10-05

## Scope and conclusion

This is a frozen, completed-cohort evaluation of the prefix-only seed-racing
policy on a materially larger Atari ST design. It is offline evidence, not an
observed online race, and it does not implement termination or
checkpoint/resume.

The result is a decisive negative gate for live stopping. Twelve of 32 runs
were legal and passed final analogue setup and hold timing on every required
clock, 14 were legal but failed that timing gate, and six hit the predeclared
600-second timeout. Across 20 scheduler orders, the prefix ranker retained no
eventual success by rank. A success survived in only six orders, always through
the independently random exploratory slot. Do not add live termination or fit
a model from this cohort.

## Frozen design and execution

The diagnostic direct-video Atari ST input was synthesized from FES revision
`31ba384656ba3f5cefecc9ee089561058f8a0918` with Yosys revision
`886afa63953e97407153e9f4aae25fcedb639696`. The frozen mapped JSON has
SHA-256 `16e052117019b375794ace17a854a75db7aa96ab81cfe31fa7dcf7b52e49a506`.
Its socket QSF and clock SDC have SHA-256
`79159cd0618cfc90980897462e7fc9f17349d4484291f932b1bec1fdacb3185e`
and `a0f6cf1636d4bf91643477d923761872edddeb64f70b9432298b1cc4c88f14bc`.

Collection used nextpnr revision
`9643627f59c2e8171164e872357219471b366cd1`, Mistral revision
`7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039`, and a native CUDA Mistral
executable with SHA-256
`aa5bf5001100d26171aaab89af5a50205d81a4fc4fb577ddbdd6948397dad0a9`.
The backend was CUDA on device 0 of an NVIDIA GeForce RTX 3090. Runs were
serial (`concurrency: 1`) so GPU contention could not change cohort behavior.

Before outcomes were observed, the plan fixed seeds 1 through 16, two
replicates, a 600-second per-run limit, a 21,600-second cohort limit, scheduler
seeds 0 through 19, stage quotas 8 then 2, and one exploratory survivor. The
required clocks were `system_clock.clocks[0]`, `pixel_clk`, and
`system_clock.clocks[1]`.

The original pilot expected rankable placement events, which this telemetry
schema does not emit. Before the pilot completed, the checkpoint rule was
amended to use the first and tenth routing-iteration arrival times. Those
prefix-only observations fixed 70- and 80-second checkpoints. The pilot was
excluded from the 32-run evaluation cohort. No checkpoint, quota, score, or
timeout was selected from cohort outcomes.

## Outcomes and repeatability

| Measurement | Result |
| --- | ---: |
| Completed, legal, all required clocks pass setup and hold | 12/32 |
| Legal, final analogue timing failure | 14/32 |
| Timed out at 600 seconds | 6/32 |
| Aggregate observed process time | 9,497.793 s |
| Completed-run time, min/median/max | 111.312/157.737/345.912 s |
| Completed-run worst-clock margin, min/max | -2.392/+0.398 ns |
| Structurally equal same-seed telemetry pairs | 16/16 |
| Byte-equal same-seed final reports and RBFs when available | 13/13 |

Seeds 1, 2, 3, 4, 10, and 12 passed in both replicates. Seeds 5 through 9,
14, and 15 completed legally but failed the all-required-clock analogue gate
in both replicates. Seeds 11, 13, and 16 timed out in both replicates. Timeout
runs correctly have no final timing report or bitstream and are treated as
censored failures, not silently dropped observations.

Structural telemetry comparison removed only `run_id`, `elapsed_s`, and
`iteration_elapsed_s`. Every same-seed pair otherwise followed an identical
trajectory. For every non-timeout pair, the timing JSON and generated RBF were
also byte-identical. This confirms repeatability for this exact serial cohort;
it does not generalize beyond the bound executable, design, constraints,
runtime dependencies, driver, GPU, and environment.

Routing legality and signoff timing remain separate facts. All 26 completed
runs routed legally. A run counted as successful only when the final analogue
report also contained both setup and hold evidence for all three required
clocks and every one passed. Telemetry never substitutes a routing proxy for
that final gate.

## Prefix-only replay

The authenticated dataset contains all 32 declared runs and no exclusions.
The adapter admits only observations available at each checkpoint; terminal
legality and final analogue timing remain unavailable until the wrapper has
completed. Consequently the 70- and 80-second stages cannot see later route,
timeout, or timing outcomes.

The random-full-run baseline used the same 9,498-second budget, the ceiling of
the cohort's observed aggregate time. It processed every run in every order
and found a success in all 20 orders. Time to first success was 111.312 seconds
minimum, 895.005 seconds median, and 2,101.842 seconds maximum.

| Policy result over 20 scheduler orders | Ideal resumable | Restart charged |
| --- | ---: | ---: |
| Orders retaining at least one eventual success | 6/20 | 6/20 |
| Recall, min/median/max | 0/0/1 of 12 | 0/0/1 of 12 |
| False rejections, min/median/max | 11/12/12 | 11/12/12 |
| Aggregate cost, min/median/max | 2,523.944/2,658.348/2,746.989 s | 3,243.944/3,378.348/3,466.989 s |

All six successful policy orders retained seed 12 replicate 2, with final
worst-clock margin +0.221 ns. It was always the stage-two random exploratory
survivor. The ranked survivor was seed 8, an eventual analogue timing failure.
Thus the observed success rate is evidence for the exploration safeguard, not
for predictive value in the current score.

The ideal-resumable cost is intentionally optimistic. The restart cost charges
every prefix attempt and then complete reruns of promoted candidates. Neither
number is presented as GPU makespan or as an online measurement. The restart
figures are the deployable comparison until a real checkpoint/resume mechanism
exists.

## Evidence and commands

The experiment directory is
`out/dev/seed-racing-milestone-e-atari`; it intentionally remains outside the
repository. Principal SHA-256 identities are:

- Cohort collection summary: `96d145cb6efd7f152d8cfad46c2b31591ecadadb740c1c3af4cb9377080d0176`
- Cohort identity fingerprint: `6387bdc44864a9a91dcbbe115a72c3c7bacd9388e0319a8ff04c417935c32de8`
- Authenticated evaluator dataset: `cce062e3ea429025e001e95a9027a7f4381d5fefc0506232c3f56e6f940db271`
- Offline evaluation: `84484aa4e08de4ca8be0e0c14cf18816f13678ba8ec48f9001752f5e73654f82`

The replay command was:

```sh
python3 python/seed_racing.py evaluate atari-dataset.json \
  --checkpoints 70,80 --quotas 8,2 --exploratory-survivors 1 \
  --scheduler-seeds 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19 \
  --budget-seconds 9498 --output atari-evaluation.json
```

## Next gate

Do not implement live termination from this result. The current prefix score
anti-correlates with the required final outcome on this design. This cohort may
be used for diagnostics or training only; it cannot become held-out evidence
for a revised score.

The next bounded step is to explain which allowlisted prefix features promoted
the failing seed 8 without changing policy on this cohort. Any revised heuristic
must be predeclared and evaluated on a new, independent mixed-outcome cohort,
with the same all-clock setup-and-hold gate, censored timeout treatment,
prefix-only feature boundary, random exploration, and honest restart costs.

This was host-only place-and-route evidence. No FPGA was programmed and no
hardware acceptance is claimed.
