# Cross-family policy screen: pre-result declaration, 2026-10-07

Follow-up to the completed, negative portfolio comparison in PR #164. This
screen changes no default, compiler algorithm, firmware, FES producer,
constraint, shared contract or hardware. It reuses the authenticated collector
and fully observed serial-run evaluator; no early stopping or checkpointing.

## Frozen population and budget

| Family | Role | Fresh seeds | Fixed timing weight | Retained baseline exponent |
| --- | --- | --- | ---: | ---: |
| Atari ST | Training | 33–36 | 2000 | 5 |
| Catch | Training | 33–36 | 10 | 7 |
| RAM-test, 100 MHz | Held out | 33–38 | 10 | 2 |
| Pong | Held out | 33–38 | 10 | 7 |
| C64, synchronous firmware and color RAM | Held out | 33–38 | 2000 | 5 |

Every family runs all four exponents **2, 5, 7, 8**, once per seed. Mistral's
actual default exponent is 7 (`mistral/arch.cc`), despite the generic CLI help
text advertising 2. Keep that native baseline rather than substituting 5 for
every family. Default timing weight is 10. Only exponent varies within each
family; weights and constraints are not weakened. `--timing-allow-fail` merely
retains negative signoff evidence, never grants a passing label.

Total bound: 32 training + 72 held-out = **104 PNR invocations**, concurrency
one, 600 seconds each, 72,000-second outer wall allowance including preparation
and automatic evaluation. No replacement seeds, retry search or undeclared
remote machines. A 12 GiB free-space guard stops before another cohort. This
bound permits 17.33 hours of worst-case PNR time; preparation/cleanup accounts
for the remaining allowance. Normal-case ETA is uncertain, approximately a
few hours based on retained runs. No paid compute or hardware programming.

Use one local RTX 3090 CUDA device, UUID
`e553e0819d33727c4728050882d7e7c1`. Keep the exact retained native binary
SHA-256 `aa5bf5001100d26171aaab89af5a50205d81a4fc4fb577ddbdd6948397dad0a9`,
source `9643627f59c2e8171164e872357219471b366cd1`. This isolates policy effects;
it is not a test of all newer C++ changes on main. Python tooling base is
`6cb41766` plus the path-resolution review fix from PR #164, absent from its
merged snapshot and carried forward here.

Mapped netlist SHA-256 identities:

| Family | SHA-256 |
| --- | --- |
| Atari | `16e052117019b375794ace17a854a75db7aa96ab81cfe31fa7dcf7b52e49a506` |
| Catch | `711932035fa2569f2e38a7f40f046b605decf193fc169582202f837c9f305918` |
| RAM-test | `f6c406ab649f52e7b8f993a184b2a717a35fa55063017ab11f7437ff4aa77686` |
| Pong | `25af49b31445d58d7ac0e93ea2348997fe68055ad10a4d46ca940c114cb3598e` |
| C64 | `d890b6f088056b52ff671ffe850c67f86f3009a47db5adf5becae0f372136056` |

Constraints, per-family required clocks, exact argv, input and binary hashes,
phase/policy order and manifest digests are sealed in the generated plan and
family declarations. Canonical plan SHA-256, published before collection:
`a33edab985c2b6395e8119b7e641e148010d6da39675588804bfe5219f710ca2`.
Evidence lives under enclosing FES `out/dev/seed-policy-multifamily/evidence`.

## Selection and comparison, before observing new routes

For each training family and policy compute successes / full observed process
seconds, including failures and timeouts. Divide each policy's rate by that
family's best rate, then average the two normalized values equally. An
all-failure family contributes zero for every policy, not an omitted group.
Rank by this mean; ties prefer exponent 5, then policy identifier. This prevents
the fast family from dominating merely because its runs take fewer seconds.

Persist the ranking and training statistics before launching any held-out
cohort. Held-out phase or family rows cannot enter selection. Netlist bytes
must be distinct across groups; renaming a previously trained mapped family
cannot create a held-out family. Numeric seed identifiers are labels only.
All prefixes/outputs of a candidate stay in the same group. Refuse missing,
duplicate, artifactless/excluded or unstarted candidates, and refuse recreating
a lost selection after held-out collection has begun.

For each held-out family compare its retained baseline policy, the frozen
single training winner, and the same fixed rotating portfolio used in #164
(now four policies). Use label-only seed permutations 0–19 and budgets
600, 1800, 3600 seconds. Report each family's counts and costs, not just pooled
passes. The primary descriptive comparison is the 1800-second budget; report
the other two without selecting a favorable budget after seeing outcomes.

Each attempted candidate pays its entire observed fresh process cost, including
placement/router initialization and timeout cleanup. Budget-crossing final
labels are unavailable. These are offline serial counterfactuals, not actual
online races, GPU utilization integrals or resumable-execution savings.

Report marginal search separately from training-charged searches. For a first
use, allocate the whole observed training bill before search. Also report
hypothetical allocations at 10 and 100 reuses: training bill / reuse count,
subtracted from the budget before any candidate may complete. The baseline
pays no training bill. If training consumes the budget, success is false without
looking at candidate labels. Ten uses is the primary amortization diagnostic;
none of these reuses have actually occurred. Cohort freezing and evaluator
overhead must be reported separately, not silently credited as saved compute.

## Interpretation and next delivery

Historical baseline results on all five fixtures are already known, including
prior Atari policy experiments. This is a held-out **new-combination** and
family-separated selection test, not a claim that the designs themselves were
never inspected. Selection sees only the new Atari/Catch training cohorts.
The small two-training/three-test split and six held-out seeds per family are
descriptive, not sufficient evidence of broad cross-design generalization.
Twenty permutations reuse the same observations and are not independent
experimental populations. Timeouts are censored, not permanent-failure proofs.

Keep legality separate from final analogue setup AND hold on all declared
clocks and the collector's table gate. Preserve missing/unavailable evidence.
C64 uses its already retained legal synchronous mapped fixture; this task does
not resume C64 integration or alter disk/hardware behavior.

The runner automatically authenticates/evaluates all completed collections and
writes a completion marker. Incomplete evidence stops the screen; no silent
reruns. Publish the tooling, synthetic tests and actual results in one coherent
PR, including negative outcomes and first-use costs. No policy/default or live
termination is authorized by a favorable descriptive screen.
