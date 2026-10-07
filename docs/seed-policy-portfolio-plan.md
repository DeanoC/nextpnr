# Bounded placement-policy portfolio screen (2026-10-06)

This is a pre-result declaration, not a claimed improvement. The seed-racing
foundation and earlier negative prefix/restart gates remain unchanged.

## Frozen experiment

Use the retained legal Atari ST mapped fixture from the 2026-10-05 gate, on
one local RTX 3090, serial CUDA runs. Mapped JSON SHA-256:
`16e052117019b375794ace17a854a75db7aa96ab81cfe31fa7dcf7b52e49a506`.
QSF and SDC SHA-256 respectively:
`79159cd0618cfc90980897462e7fc9f17349d4484291f932b1bec1fdacb3185e`,
`a0f6cf1636d4bf91643477d923761872edddeb64f70b9432298b1cc4c88f14bc`.
Keep the existing CUDA binary from source `9643627f59c2e8171164e872357219471b366cd1`,
SHA-256 `aa5bf5001100d26171aaab89af5a50205d81a4fc4fb577ddbdd6948397dad0a9`.
This is deliberately not a newly built binary: isolate placement-policy effects.
Collector/evaluator base is `da3c7fff5b85c1ab5da1fa704df0a8e7c94e8c1e`.

Only criticality exponent varies: 2, 5 (baseline), 8. Timing weight stays 2000.
All other argv, input, required clocks and CUDA settings stay fixed. Required
clocks are `system_clock.clocks[0]`, `pixel_clk`, `system_clock.clocks[1]`.
Routing legality is independent of final analogue timing: success requires
legal routing and strictly positive setup AND hold margins on every required
clock, with the authenticated collector's additional timing gates.

Training seeds 17–24: 24 runs. Held-out seeds 25–32: 24 runs. Seed 17 repeats
once under each exponent: 3 more runs. Maximum 51 PNR invocations, 600 seconds
each, concurrency one. Nine-hour outer wall allowance includes preparation.
Stop before another cohort if fewer than 12 GiB remain. No replacement of
failed/incomplete collections and no opportunistic extra seeds.

Canonical declaration SHA-256, published before collection:
`7328f9c8c403768b4954633837ca20cdcfaf135a0d6d83d15898291d332f647d`.
Generated manifests and evidence live outside git under
`out/dev/seed-policy-portfolio/evidence` in the enclosing FES worktree.

## Predeclared comparison

Rank policies by training successes divided by total observed training seconds,
including failures/timeouts. Ties prefer baseline, then policy identifier.
Persist this selection before collecting any held-out routes. Held-out labels
or times never enter this selection. Seed identifiers are labels, not numeric
predictor inputs.

On held-out full-run evidence compare ordinary exponent-5 runs, training-selected
single-policy runs, and a fixed rotation of all three training-ranked policies.
Use SHA-256 seed permutations for scheduler seeds 0–19 and budgets 600, 1800,
3600 seconds. All methods use the same seed permutation. The rotation tries one
policy per seed per lap, rotating policies across laps; each candidate appears
once. No prefix-derived ranker, partial-run predictor, or checkpoint resumption.

Charge full observed execution cost for each completed attempt, including
restart initialization and timeouts. A run crossing a budget consumes the
remaining budget and its final label is unavailable. Stop a simulated schedule
at its first completed legal all-clock success. These schedules are offline
counterfactuals, not observed live races. Report training, repeat and collection
costs separately; a first deployment must also pay training cost. Twenty
permutations are not twenty independent experimental populations.

This is a descriptive single-family screen. Eight held-out seeds do not establish
cross-design generalization or authorize default changes/live termination.
Check seed-17 report/RBF repeatability when both anchors complete; two censored
runs do not establish equal final results. Publish negative outcomes as well.

## Execution

`python/seed_policy_portfolio.py` wraps the existing authenticated collector.
Prepare using the retained Atari manifest, inspect the declaration/dry-run, then
run with the nine-hour outer timeout. Evaluate only after all declared cohorts
exist. Synthetic tests cover training-only selection, failed-run cost, tie
breaking, deterministic candidate enumeration, and budget censoring/restarts.
No hardware programming, C64 integration, paid compute or default changes.
