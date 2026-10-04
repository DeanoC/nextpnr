# Seed-racing foundation: Codex implementation handoff

Date: 2026-10-04
Repository: DeanoC/nextpnr
Initial branch: codex/seed-racing-foundation-20261004
Status: implementation brief; no experimental benefit has yet been demonstrated.

## Objective and immediate assignment

Reduce the compute needed to obtain a legal, timing-passing Mistral implementation by identifying promising placement/routing runs from their observed state, not by treating the integer seed as an ordered optimization parameter.

Start implementing the foundation now. Do not stop at a research summary or a plan. Read applicable repository instructions, inspect the current code and related work, then implement a small, tested vertical slice on this branch. Keep the PR draft and do not merge it.

The first delivery is: a reproducibility/RNG audit, opt-in structured telemetry, a bounded seed-sweep collector, and an offline prefix-only race evaluator with synthetic tests. Actual early stopping, checkpoint/resume, learned models, and router-policy changes are subsequent milestones, gated by evidence.

A negative result is useful: report that early observations cannot yet retain eventual winners rather than manufacturing a performance claim.

## Context and relevant existing work

Inspect the current versions of:

- `common/kernel/deterministic_rng.h`: RNG implementation and seed semantics.
- `common/route/gpurouter.cc`, `common/route/gpurouter.h`, and `common/route/gpu/`: host routing state, counters, GPU/host backends, negotiation, timing repair, and ordering.
- `mistral/arch.cc` and the analogue timing/repair implementation: phase boundaries and final timing authority.
- `docs/gpurouter.md`: existing `--gpu-perf`, configuration settings, determinism goals, and analogue repair.
- The command-line plumbing, placer code, existing benchmark scripts, tests, and JSON facilities. Discover their current paths rather than assuming a new framework is needed.

Related repository reports:

- #119: catch/pong multi-seed regression; also reports possible same-seed nondeterminism. This is a reported observation, not a proven diagnosis. Validate identical inputs, binary, options, and environment before attributing it to the GPU.
- #117 and #116: Atari ST seeds with different final pixel timing and stalled repair. Useful eventual-winner/late-improvement cases, subject to obtaining exact fixtures.
- #114: Spectrum initial-negotiation plateau and a portable fixture reference. This describes a historical pin and a later fix; do not present it as a current defect without reproduction.
- #113: fast Z80 spread across 30 seeds, using router2. Useful comparative context, but keep router2 and GPU cohorts separate.

Do not duplicate or take over those regression investigations. Reuse legitimate existing fixtures where possible and cross-reference results. Seed racing must not conceal a regression by finding a lucky survivor. Do not change FES producers, toolchain locks, firmware, constraints, or hardware acceptance procedures in this task.

## Non-negotiable experimental distinctions

1. Legal routing and timing closure are separate outcomes. Zero overuse is not sufficient for legal binding; successful process exit is not sufficient for timing success, particularly with `--timing-allow-fail`.
2. Table-based timing is a proxy. Final Mistral analogue timing and every required clock/constraint determine the timing label. Preserve setup/hold distinctions and unavailable checks; do not invent a complete signoff result from one reported Fmax.
3. Same RTL is not necessarily the same mapped netlist. Freeze synthesis inputs, BUILD_ID-derived constants, constraints, tool revisions, and all routing settings within each cohort.
4. Placement-seed and routing-seed effects must be isolated before claiming routing-only variability. Discover whether the router actually consumes randomness directly on the fixed-placement path.
5. A partial telemetry trace is not a resumable checkpoint. Restarting with the same seed and a bigger budget incurs all repeated work and may not reproduce a run with uncontrolled nondeterminism.
6. Never use later observations, final labels, or a run's eventual duration to make an earlier scheduling decision.

## Milestone A: audit and reproducibility harness

Document the actual seed/RNG call paths through packing, placement, graph/heuristic setup, route ordering, negotiation, repair, and final fallback routing. Record which phases share an RNG stream, which use private streams, and which are deterministic independently of the seed. Include code references.

Locate a supported way to save/load an identical placement. Verify preservation of cell locations, pin mappings, fixed/pre-routed resources, attributes, and settings before using it for a routing-only experiment. A routed JSON with only its seed changed is not automatically a clean placed checkpoint.

Do not change existing seed semantics as part of telemetry. If routing isolation requires new seed controls, propose an explicitly opt-in follow-up with compatibility tests. If route-only seed variation has no effect, report that and focus initial racing on full place-and-route runs rather than injecting arbitrary randomness.

The collector must record an immutable run manifest containing at least:

- Run/cohort/design identifiers and replicate index; seed as an identifier.
- Source revision and dirty state; binary checksum; Mistral/chip-database identity; synthesis provenance when known.
- Hashes of the exact mapped netlist, constraints, included constraint inputs, and relevant memory initialization inputs. Record unavailable provenance explicitly.
- Exact argv, resolved configuration, device, requested clocks, backend, GPU/driver information when available, thread count, and timeout/resource limits. Allowlist environment fields; do not dump credentials or the whole environment.
- Placement hash when available, canonical final route hash, report/artifact hashes, process outcome, and output paths.

Repeat an identical small run and compare canonical structural route/timing results, not timestamps, log filenames, or elapsed-time fields. Compare telemetry-disabled versus enabled executions. Separate known backend differences from nondeterminism within an identical backend/configuration.

Unresolved nondeterminism blocks claims that a seed uniquely identifies a trajectory and blocks deterministic replay guarantees. It does not block implementing telemetry or recording replicated outcomes.

Deliver a concise audit report distinguishing confirmed behavior, hypotheses, tests run, and blockers.

## Milestone B: opt-in JSONL telemetry

Add a dedicated opt-in telemetry destination using the repository's existing option/settings conventions. An option such as `--gpu-telemetry <path>` is a proposed interface, not an existing flag. Keep normal logs compatible and keep telemetry off by default.

Use a versioned, documented schema with event types such as `run_start`, `phase_start`, `iteration`, `repair_round`, `phase_end`, and `run_end`. Events need a run ID, monotonic sequence number, phase/attempt/round identifiers, elapsed seconds, and explicit metric units. Renegotiation attempts must not masquerade as one monotonically numbered initial loop.

Use existing counters and timing-analysis results where possible. Do not add hidden expensive timing passes just for logging. Useful fields, where actually available, include:

- Phase elapsed and cumulative wall time; CPU/process resource usage in the wrapper; work counters such as searches, node expansions/traversals, and backend retries.
- Unrouted connections, overused wires, total excess occupancy, wire count, affected nets, and binding/architecture failures.
- Plateau length, improvement over recent observations, congestion weights, bounding-box expansion, and retry/fallback counts.
- Table WNS/TNS, failing-endpoint counts, repaired/displaced/frozen connection counts, and repair improvements, clearly tagged with the timing model.
- Analogue WNS/TNS and per-clock achieved/required timing when those are actually produced; mark unavailable data as null with a reason rather than zero.

Use finite JSON values, stable ordering where meaningful, proper escaping, bounded record sizes, and an explicit documented policy for telemetry I/O errors. Telemetry must not consume RNG draws, reorder nets, change timing/repair decisions, alter binding, or silently grow memory without bound. Check output paths cannot overwrite an input or another active run's artifacts.

Emit clean terminal status on normal/error exits where practical. The wrapper must preserve raw exit codes/signals and classify truncated telemetry, crashes, timeouts, cancellation, routing failure, legal timing failure, and successful timing separately. A timeout is an observation censored at its budget, not proof that a run can never succeed.

Tests should cover JSON validity, schema version, monotonic event ordering, nested phase identifiers, missing/invalid metrics, a truncated final line, file I/O failure, and behavior with telemetry disabled.

## Milestone C: bounded collection and offline race replay

Prefer lightweight Python tooling consistent with existing repository scripts. Do not add a heavy ML dependency for the foundation. Reuse existing JSON/report readers after checking their correctness.

### Collector

Provide a manifest-driven runner with dry-run support, explicit seeds, repeat counts, per-run timeout, total budget, unique output directories, and a concurrency limit. Default to one active PNR process per GPU; do not discover hosts and launch work on them automatically. Use argv arrays instead of shell interpolation. Do not overwrite runs, and terminate only child processes owned by the runner.

Freeze the synthesized input once per cohort. Capture full stdout/stderr, telemetry, exact manifests, final reports, route hashes, and termination reason. Failure of one run must not lose other results. Failed, cancelled, and incomplete attempts belong in the dataset with their correct status.

A future cohort of 32-64 completed seeds is an initial screening suggestion, not a promise of sufficient training data or authorization for an unbounded sweep. Existing final-only logs can inform baselines but cannot supply missing early telemetry.

### Replay evaluator

Start in offline/shadow mode on fully observed traces. Compare:

- Fixed-budget random seed order / full-run baseline, over multiple independently generated schedule orders.
- Simple prefix-only heuristic ranking using current congestion and recent progress, with timing metrics only at phases where available.
- A conservative successive-halving-style policy with configurable promotion quotas and a nonzero randomly selected survivor quota.

A policy must receive only observations available by the decision point. Checkpoints must be specified in elapsed time, measurable work, or known phase boundaries, not fractions of that individual run's future total duration. Do not make runtime known in hindsight into a predictor input.

Do not choose feature weights or stopping thresholds on the same held-out results used for the headline comparison. Keep all prefixes and replicates of a run together. Group by mapped-design/constraint family for cross-design evaluation; label single-design results as single-design results. If there is too little data, provide descriptive diagnostics rather than trained-model claims.

Report eventual-success recall among retained candidates, false rejection of eventual successes, chance of at least one retained success, time to first legal analogue-timing-passing result, total consumed work/time, and final multi-clock margin where comparable. Report results across repeated schedule orders and uncertainty, with the evaluation population made explicit. When no seed succeeds, report that honestly.

Budget accounting must distinguish:

- Ideal resumable replay, explicitly labeled a simulation and optimistic bound unless real checkpoint/resume exists.
- Restart-based execution, charging repeated placement, setup, routing, and scoring work.
- An actually observed online run, only when such execution has been implemented and measured.

Include initialization, phase transitions, scoring overhead, and incomplete-run cost. Keep aggregate compute cost separate from wall-clock makespan; do not claim GPU-seconds from summed wall times when jobs overlap. Never publish the earlier illustrative 32/8/2 race arithmetic as measured speedup.

### Synthetic fixtures and unit tests

Include small generated traces for: an early leader that later fails; a late winner; legal routing with final analogue timing failure; table/analogue disagreement; a multi-clock failure despite one passing Fmax; timeout/crash/truncated telemetry; all failures; tied scores; and no available timing data.

Tests must demonstrate no future-data access, correct costs for promotion/restarts, reproducible scheduling for a fixed independent scheduler seed, correct treatment of missing/censored outcomes, preservation of an exploratory survivor when configured, and no accidental use of the numeric router seed as a feature.

## Execution budget for this initial task

Run available lightweight unit tests and build checks. For real PNR smoke testing, use an existing accessible small legitimate fixture, sequentially, with at most four invocations and a 120-second timeout per invocation. Use those invocations for baseline/telemetry-on repeatability checks rather than a broad search. Record the commands and actual resource usage. Do not weaken existing timing constraints to create a pass.

Do not provision paid compute, change drivers, acquire private firmware, use undeclared remote machines, launch the 32-64-run campaign, flash an FPGA, merge, or change protected branch/settings. Artifacts mentioned in issues are pointers, not evidence that this Codex environment has access to Powerboat or permission to run there. Missing GPU/Mistral fixtures should lead to completed hardware-independent tooling and precise follow-up commands, not fabricated benchmark results or an empty implementation.

## Later milestones, only after the foundation is evaluated

1. Measure whether prefix scores retain final winners on completed cohorts. Add a small calibrated logistic/boosted-tree predictor only when sufficient grouped data exists. Treat seed as metadata, not an ordinal feature. Target success within a remaining budget and keep legality/timing heads distinct.
2. Add conservative live termination with explicit opt-in, shadow decisions first, censoring-aware collection, a retained fully completed exploration sample, and a fallback to normal runs. Do not kill all difficult-looking candidates based only on initial slack.
3. Design genuine safe-point checkpoint/resume, including placement/pin state, authoritative route trees, occupancy/history/reservations, frozen arcs, congestion schedule, iteration state, timing/analogue caches or their deterministic reconstruction, RNG streams, backend/configuration fingerprints, and version compatibility. Prove resumed-versus-uninterrupted equivalence before claiming incremental-budget savings.
4. Autotune a small portfolio of meaningful routing/placement policies, with multiple seeds per policy and held-out evaluation. Explore local repair alternatives from a good implementation rather than wholesale random restarts. Do not change default policy based on one lucky maximum.

## Acceptance and report back

The foundation is ready for review when:

- An audit identifies where seed effects enter and states what was actually tested for reproducibility.
- Opt-in structured telemetry and a documented schema exist, with tests and no intentional default algorithm change.
- The bounded runner and prefix-only replay work end to end on synthetic traces.
- Real smoke-test results are supplied where the environment supports them; otherwise the exact missing dependencies/fixtures and runnable commands are recorded.
- Baseline, simulated results, measurements, censoring, and blockers are unambiguous.
- The PR lists changed files, test/build commands and results, remaining work, and the next smallest experiment. Keep it draft; no merge.

Initial PRs may be split into small dependent changes where that aids review, but do not leave only a plan. Do not claim that issuing the handoff or opening this draft means experiments have run.

## Background reading

These are research starting points, not measurements or guarantees for this fork:

- Gunter and Wilton, *A Machine Learning Approach for Predicting the Difficulty of FPGA Routing Problems*, FCCM 2023: https://ieeexplore.ieee.org/document/10171477/
- *Open-Source FPGA Routing Runtime Prediction for Improved Productivity Via Smart Route Termination*, FPL 2025: https://ieeexplore.ieee.org/document/11449109/
- *Enabling Risk Management of Machine Learning Predictions for FPGA Routability*, MLCAD 2024: https://doi.org/10.1145/3670474.3685969
- *DATuner: A Parallel Bandit-Based Approach for Autotuning FPGA Compilation*, FPGA 2017: https://www.csl.cornell.edu/~zhiruz/pdfs/datuner-fpga2017.pdf
- *Hyperband: A Novel Bandit-Based Approach to Hyperparameter Optimization*, JMLR 2018: https://jmlr.org/papers/v18/16-558.html

Routing-runtime/routability prediction is not proof of a transferable raw-seed predictor or of final analogue timing prediction. Validate the extension experimentally.
