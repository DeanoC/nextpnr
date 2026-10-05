# Seed-racing Milestone D evaluation, 2026-10-05

## Scope and conclusion

This is a completed-cohort, offline evaluation of the seed-racing foundation
merged at `27170f0a41d5db195e79dfe70be0cd8cb63166b0`. It is not an observed online
race and does not implement termination or checkpoint/resume.

The result is negative for live racing on these fixtures. Every Catch and Pong
run was legal and passed final Mistral analogue timing on every required clock.
A normal full run therefore finds a success in about 9 or 11 seconds, while a
race must spend hundreds of aggregate seconds observing all candidates before
it can select survivors. Do not enable live stopping from this evidence.

## Frozen cohorts

Both cohorts used the native `BUILD_PYTHON=OFF` CUDA Mistral executable with
SHA-256 `226f59f01dc19ffe35ac59897d346b5e83eccb05fca897d5c9cf602d3e053000`,
source revision `27170f0a41d5db195e79dfe70be0cd8cb63166b0`, and exact backend
`cuda:e553e0819d33727c4728050882d7e7c1:0000:01:00.0:NVIDIA GeForce RTX 3090`.
Collection was serial (`concurrency: 1`) with a 120-second per-run limit and
1,800-second cohort limit. Each design used seeds 1 through 16 with two
replicates, for 32 full PNR runs per design and 64 total.

Catch bound mapped netlist
`711932035fa2569f2e38a7f40f046b605decf193fc169582202f837c9f305918`
and required `core.game.clk` plus `audio.clk`. Pong bound mapped netlist
`25af49b31445d58d7ac0e93ea2348997fe68055ad10a4d46ca940c114cb3598e`
and required `core.game.clk`. Constraints, runtime dependencies, driver
identity, environment, command, reports, telemetry, and bitstreams are bound by
the collection cohort identities.

## Outcomes and repeatability

| Measurement | Catch | Pong |
| --- | ---: | ---: |
| Completed/legal/all-clock timing pass | 32/32 | 32/32 |
| Aggregate observed process time | 289.153 s | 363.620 s |
| Per-run time, min/median/max | 8.667/9.051/9.362 s | 11.025/11.284/12.545 s |
| Final multi-clock margin, min/median/max | 0.727/0.780/0.925 ns | 0.142/0.738/0.781 ns |
| Same-seed replicate bitstreams equal | 16/16 | 16/16 |
| Same-seed final reports equal | 16/16 | 16/16 |
| Same-seed structural telemetry equal | 16/16 | 16/16 |
| Distinct bitstreams across 16 seeds | 16 | 16 |

Structural telemetry comparison removed only run identifiers and elapsed-time
fields. Event structure, counters, occupancy, work totals, timing proxies, and
terminal fields matched for both replicates of every seed. Wall time did vary,
so a fixed elapsed checkpoint can expose one more iteration in one replicate
without implying a different routing trajectory.

This controlled result does not reproduce the same-seed nondeterminism concern
from issue #119. It narrows the claim to this exact binary, mapped input,
constraints, environment, backend, and serial execution. It does not invalidate
the historical report under a different or incompletely recorded cohort.

## Prefix-only replay

The collection-to-dataset adapter verifies persisted result and telemetry
digests. It maps only iteration/repair records into allowlisted observations,
derives recent progress from already observed excess occupancy, and withholds
`run_end` legality and final analogue timing until full wrapper completion.
Catch and Pong were evaluated separately as single-design populations over 20
independent `sha256-order-v1` scheduler seeds. Each policy retained one ranked
and one independently exploratory survivor.

At the early fixed checkpoints:

| Policy result (median over 20 schedules) | Catch, 7.0 s | Pong, 7.5 s |
| --- | ---: | ---: |
| Prefix-visible runs | 32/32 | 29/32 |
| Ideal-resumable aggregate cost | 227.741 s | 247.670 s |
| Restart-charged aggregate cost | 241.741 s | 262.670 s |
| Retained eventual-success recall | 2/32 (6.25%) | 2/32 (6.25%) |
| At least one retained success | 20/20 | 20/20 |
| Random-full-run time to first success | 9.057 s | 11.343 s |

Because all candidates eventually succeed, “at least one success” is not
evidence that the prefix score adds value. The early score consistently ranked
Catch seed 7 replicate 2 and usually ranked Pong seed 5 replicate 1; it did not
select the best final Catch margin.

Later checkpoints at 8.0 seconds for Catch and 10.0 seconds for Pong made all
32 candidates prefix-visible. Restart-charged median costs rose to 274.128 and
342.687 seconds. The ranked candidates were Catch seed 4 (0.806 ns final
multi-clock margin versus the 0.925 ns population best) and Pong seed 8
(0.774 ns versus the 0.781 ns best). Random exploration occasionally retained
the population best, but that is deliberate exploration rather than predictive
evidence.

The ideal-resumable figures are optimistic simulations. The restart figures
charge every prefix and then the survivors' complete reruns. Neither is GPU
makespan or an online measurement.

## Evidence and commands

The uncommitted experiment directory is
`out/dev/seed-racing-evaluation-20261005`; it intentionally remains outside the
repository. Principal SHA-256 identities are:

- Catch collection: `1d8a1057092d61ebd44a42f6ce877a8a79c1f729a441e90f3fd4522b597c5016`
- Pong collection: `91dcecc3e561626fc502ae9cc7c97ead3fc1dbea1b0ce5061e2e6e135d6584dd`
- Verified Catch evaluator dataset: `0be931ed976ebe9371675dd891ad04e981a6c1d0170e2b9169a6821b9dfe97db`
- Verified Pong evaluator dataset: `6a958548a57bd3c9bb011ce246febff27a54476aa8d302b465898877a5f61914`

Representative replay commands were:

```
python3 python/seed_racing.py evaluate catch-dataset-verified.json \
  --checkpoints 7 --quotas 2 --exploratory-survivors 1 \
  --scheduler-seeds 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19 \
  --budget-seconds 290 --output catch-evaluation.json

python3 python/seed_racing.py evaluate pong-dataset-verified.json \
  --checkpoints 7.5 --quotas 2 --exploratory-survivors 1 \
  --scheduler-seeds 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19 \
  --budget-seconds 364 --output pong-evaluation.json
```

Focused validation after adding the adapter:

```
python3 -m unittest mistral.tests.seed_racing_test
# Ran 82 tests ... OK
python3 -m py_compile python/seed_racing.py
git diff --check
```

## Next gate

Do not add live termination yet. Evaluate a legitimate, frozen design cohort
with a meaningful mix of failures, late successes, or materially different
final margins. Predeclare checkpoints and quotas rather than tuning them on the
same outcomes. Only then consider a shadow online decision logger; actual
termination remains gated on retained-success recall and honest restart cost.
