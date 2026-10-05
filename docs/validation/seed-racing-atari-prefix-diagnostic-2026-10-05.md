# Atari seed-racing prefix diagnostic, 2026-10-05

## Scope and conclusion

This is a post-gate diagnostic of the frozen Atari ST cohort recorded in
`seed-racing-milestone-e-atari-2026-10-05.md`. The final outcomes are visible
to this analysis, so the cohort is now training/diagnostic data. Results here
must not be presented as held-out validation of a revised policy.

The original score was effectively one-dimensional on this cohort. Its first
lexicographic component, total excess occupancy, differed across seeds at both
checkpoints. Later components therefore could not affect cross-seed order.
Seed 8 had the least congestion at 70 seconds and reached zero excess occupancy
by 80 seconds, so it always won the final ranked slot. Its prefix table WNS was
already -1.871 ns, and it eventually failed final analogue timing at -2.196 ns.

The correction is not a claim that prefix table timing predicts signoff.
`balanced-prefix-v1` merely preserves timing and routability as separate ranked
lanes instead of allowing one to erase the other. A new independent cohort is
required before even a shadow online decision logger is justified.

## Why the original ranking failed

`congestion-first-v1` orders candidates by:

1. least `total_excess_occupancy` (or `overused_wires` when unavailable);
2. least `unrouted_connections`;
3. most `recent_progress`;
4. best `table_wns_ns`.

At 70 seconds, `unrouted_connections` was unavailable for every Atari run.
Because excess occupancy differed among seeds, neither recent progress nor
table WNS participated in their order. The first seven ranked promotion slots
were consumed by both repeats of failing seeds 8, 15, and 14 plus one repeat of
successful seed 12. At 80 seconds, failing seed 8 had zero excess occupancy and
again ranked first.

The contrast was already visible in prefix-only data. At 70 seconds successful
seeds 1 and 10 had the two best seed-level table WNS values, -0.027 ns and
-0.093 ns, but excess occupancies of 30,302 and 3,839. Failing seed 8 had only
397 excess occupancy but table WNS -1.871 ns. Reaching legality early did not
imply passing final analogue timing on all required clocks.

## Replicate-safe sensitivity replay

The earlier 32-run replay allowed two executions of the same numeric seed to
compete in one simulated race. That population remains an explicit historical
result, but it is not representative of launching each seed once. The new
`--replicate-stratified` mode evaluates repeat 1 and repeat 2 as separate
16-candidate races. It rejects duplicate seed candidates within a stratum and
applies the fixed budget independently to each race.

Using the original 70/80-second checkpoints, quotas 8 then 2, one random
exploratory survivor, scheduler seeds 0 through 19, and a fixed 4,751-second
budget per stratum produced:

| Restart-charged result | Repeat 1 | Repeat 2 |
| --- | ---: | ---: |
| `congestion-first-v1`: orders with any success | 9/20 | 8/20 |
| `congestion-first-v1`: ranked final survivor | seed 8 failure, 20/20 | seed 8 failure, 20/20 |
| `congestion-first-v1`: median cost | 2,261.043 s | 2,269.900 s |
| `balanced-prefix-v1`: orders with any success | 20/20 | 20/20 |
| `balanced-prefix-v1`: ranked final survivor | seed 1 success, 20/20 | seed 1 success, 20/20 |
| `balanced-prefix-v1`: median recall | 1/6 | 1/6 |
| `balanced-prefix-v1`: median false rejections | 5/6 | 5/6 |
| `balanced-prefix-v1`: median cost | 2,222.543 s | 2,221.374 s |
| Random full-run median time to first success | 581.187 s | 580.570 s |

The original ranked policy still retained no success; every success came from
random exploration. Stratification changes the exploration probability because
the race has 16 rather than 32 candidates, but it does not change that causal
finding.

The balanced policy alternates ranked slots between the best available prefix
table WNS and the best congestion score, beginning with timing and skipping
duplicates. If no candidate has a finite prefix table WNS, congestion fills the
slots. Numeric router seeds, seed-bearing run IDs, future observations, final
legality, and final analogue timing are not ranking features. Exploration still
selects independently from the unranked remainder.

Its perfect training-cohort success rate is not validation. It selected seed 1
because this already-inspected cohort makes seed 1's prefix timing attractive.
Moreover, its roughly 2,222-second median restart cost remains much greater
than the roughly 581-second random baseline time to first success. Nothing here
supports live termination or a speedup claim.

The authenticated diagnostic outputs, retained outside the repository beside
the original cohort, have SHA-256:

- Stratified congestion-first replay: `78bb4590b0de6c47c72d837711b5e378c63410fd3fabd90f5f581f4b20ec046a`
- Stratified balanced replay: `6730eb6e9c91d620f60a31d1553fa0a8501db2a251037b2d717ff6faa9d0e0d7`

## Predeclared held-out gate

The next cohort will use the FES C64 core, not Atari ST. Before its outcomes are
observed, freeze the mapped netlist, constraints, nextpnr executable, runtime
closure, GPU/backend identity, source revisions, environment, and all required
clock names. Use one excluded pilot only to set checkpoints: take the first and
tenth rankable routing-iteration arrival times and round each upward to the next
10 seconds. If they collapse to one value, advance the second by 10 seconds.

After the pilot, collect seeds 1 through 16 with two serial repeats, a
600-second per-run limit, and a 21,600-second total collection limit. Replay
each repeat as its own stratum with:

- frozen `balanced-prefix-v1` ranking;
- the two pilot-derived checkpoints;
- quotas 8 then 2;
- one random exploratory survivor;
- scheduler seeds 0 through 19;
- a fixed 9,600-second budget per stratum, the declared worst-case full-run
  cost rather than a value selected from observed outcomes;
- success only for legal routing plus positive final analogue setup and hold
  margins on every required clock.

The gate to a shadow decision logger requires at least one **ranked** eventual
success in at least 18 of 20 scheduler orders in both replicate strata. Report
exploratory successes separately, along with recall, false rejections, random
time to first success, ideal-resumable cost, and restart-charged cost. A passing
ranking gate permits shadow logging only; it does not authorize termination.

If the C64 cohort lacks both successes and failures, report it as
non-discriminating and do not tune checkpoints, constraints, or policy on its
outcomes. If either stratum misses the ranked-success gate, stop policy work and
do not implement live termination. All work remains host-only; no FPGA
programming or hardware acceptance is part of this gate.
