# Seed-racing collection and offline replay

`python/seed_racing.py` is a standard-library-only foundation for bounded
collection and shadow evaluation. It does not stop a live PNR run or provide a
checkpoint/resume mechanism.

## Collection manifest

Run `python3 python/seed_racing.py collect MANIFEST --output DIRECTORY` or add
`--dry-run` to inspect the expanded argv without creating output. The JSON
manifest has schema version 1 and contains:

- `cohort`: `id`, `design_id`, `mapped_design_id`, and `constraint_family`;
- `architecture`: an explicit architecture identifier (including
  `himbaechel` when that frontend is used), independent of the binary name;
- `command`: an argv string array, never a shell command;
- `seeds`, `repeats`, and `limits` (`per_run_seconds`, `total_seconds`, and
  `concurrency`);
- `inputs`: exact mapped netlist, constraints, included constraints, memory
  initialization, or other inputs to hash, each with a non-empty semantic
  `role`;
- `required_clocks`: every clock whose final timing constraint must pass,
  using the exact final-report clock names;
- `artifacts`: names mapped to paths relative to each unique run directory;
- an explicit environment allowlist and provenance object containing
  `source_revision`, boolean `dirty`, and an immutable
  `runtime_environment_id: auto`; the collector replaces it with a derived
  SHA-256 manifest for the ELF loader, linked libraries, available
  NVIDIA driver identity, OS release, and
  runtime share tree. Children receive
  only that map plus the recorded platform-minimum environment (`PATH`,
  deterministic `LC_ALL=C`); ambient GPU visibility, thread-count, loader, and
  routing controls are not inherited. Explicit `LD_*` and `GLIBC_TUNABLES`
  settings are rejected because they can inject code outside the sealed closure.

Command arguments may use `{seed}`, `{repeat}`, `{run_id}`, `{run_dir}`,
`{telemetry}`, and `{report}`. The collector creates every run directory
exclusively and opens every declared output before launch. Worker output argv
is rewritten to inherited `/proc/self/fd` paths; after the process group is
quiescent, the collector hashes those same open objects and rejects any removed
or replaced display path. This prevents a pathname swap between worker exit and
evidence capture. The collector captures stdout/stderr, preserves exit
codes/signals/timeouts, and terminates only the fresh
child process group it owns. Cancellation gates future launches before it
terminates active groups, waits through the graceful interval, forcibly clears
remaining descendants, and reaps the leader, so queued or forked work cannot
continue after interruption. The collector writes the complete terminal
summary before propagating an interrupt.
`--randomize-seed` is forbidden because it would replace the seed bound by the
manifest while leaving the result labeled with the planned value.
Declared cohort inputs must exist and be regular files. On Linux they are
copied once before submission into sealed in-memory descriptors and passed to
every child through `/proc/self/fd` paths. The readable files beside collection
output are display copies, not the bytes workers consume. The collector checks
source identity, size, and timestamps around each copy and fails if a source
changes during capture. Exact input-path argv entries, equivalent resolved path
spellings, and `--option=PATH` values are rewritten to those descriptors. An
input that cannot be snapshotted, or is declared but not bound into every
worker command, aborts the cohort before submission. A missing output is
recorded as unavailable.

Known nextpnr input-bearing options (`--json`, constraint formats, chipdb,
and read input) must resolve to declared inputs with their
corresponding roles (`mapped_netlist`, `constraints`, `chipdb`, `design_input`,
or `timing_report`). Remap-plan options are rejected because their nested report
paths cannot be redirected safely. Python hooks are rejected because their
transitive module/resource imports cannot be bounded by a direct file snapshot.
Mistral's experimental
`NEXTPNR_MISTRAL_*` environment controls are rejected because some encode
untyped path/prefix inputs. Nested includes are not discovered automatically; use
flattened direct input files or treat that collection as unsupported.
Any other existing file named directly in argv must also be declared, even when
its option is not in the known schema; positional Python scripts are rejected.
Seed-racing collection currently accepts only native `nextpnr-mistral` cohorts,
because the generic architecture does not emit authoritative final-analogue timing.
The manifest architecture must be `mistral`. Collection must bind exactly one
explicit `--router`; `gpu` additionally requires exactly one
`--gpu-telemetry {telemetry}` binding, exactly one `--rbf {bitstream}` binding,
and exactly one `--report {report}` binding. The corresponding `telemetry`,
`bitstream`, and `final_report` artifacts must all be declared so the collector
can descriptor-bind them. The RBF binding is required because Mistral performs
authoritative final-analogue signoff only while building the bitstream. These bindings must occur before
`--`. JSON/read inputs whose settings override the declared router are rejected.
`router1` and `router2` collections are rejected because they do not emit the
terminal structured legality evidence required for a complete seed-racing
outcome. HIP runtimes older than version 6 remain
usable for ordinary GPU routing, but their telemetry is explicitly unattested
because those runtimes do not expose the exact device UUID; seed-racing
collection rejects that backend identity.

The command executable must implement the versioned native seed-racing contract,
must be built with `BUILD_PYTHON=OFF`, is resolved, and
is copied once into a sealed descriptor
before workers are submitted. Every run executes that descriptor and records
its original path, display snapshot path, descriptor launch path, and SHA-256.
Before accepting the cohort, the collector invokes `--seed-racing-contract`
through the sealed loader, executable, and dependency descriptors and requires
the exact versioned response; a matching filename or inert marker is insufficient.
On Linux nextpnr installations, an executable-relative share tree is copied into
the frozen cohort runtime, made read-only, content-bound in runtime evidence,
watched recursively for any write/attribute/name mutation (including a later
restore), re-enumerated after all workers, and selected through an inherited
directory descriptor in `NEXTPNR_EXECUTABLE_DIR`; replacing its display path
cannot redirect later workers. Collection fails closed without Linux
sealed descriptors, pidfds, and `/proc`. The executable's dynamic dependency
closure is copied and content-checked into individually sealed descriptors before
submission. Every dependency must have a `DT_SONAME` exactly matching the
`DT_NEEDED` name that resolved it; otherwise collection rejects the runtime.
Each worker is invoked by the sealed dynamic-loader descriptor with those
name-bound sealed library descriptors explicitly preloaded, so the loader does
not reopen a mutable dependency pathname and later swaps cannot alter the bytes
consumed by another worker. The command must be a native
`nextpnr` ELF executable; Python-enabled nextpnr builds, shebang commands,
standalone language interpreters, generic launchers, and nextpnr Python hooks
are rejected because their implicit module/resource search trees cannot be
bounded.
A recognized statically linked ELF binds the executable itself without inventing
a loader dependency. Dynamic closure freezing currently requires the glibc
`ld-linux` interface and fails closed for another loader. Runtime content identity is derived before submission and
verified again after all workers finish; any change rejects the cohort. The collector keeps
the raw process lifecycle in `process_status`, then classifies the run from
the `telemetry` and `final_report` artifacts. A valid terminal `run_end`
determines routing legality and separately records the table-model timing gate;
a legal route that misses that enabled gate is `timing_constraint_failure`. Every declared required clock must have finite
final report evidence; a failing clock is `analogue_timing_failure`, an
illegal route is `routing_failure`, and missing or truncated evidence is
`incomplete_evidence`. A standard nextpnr `fmax` report can establish a
setup failure, but cannot establish success because it lacks hold evidence. The
built-in `timing_summary` supplies exact setup and hold WNS, but the collector
accepts it as final only when `final_analogue_model` is true after Mistral RBF
signoff; table-model reports remain incomplete. A normalized
`outcome.analogue_clocks` report may also supply both values. Zero slack fails,
matching nextpnr's strict timing gate; constraints ignored by the configured timing gate are excluded from the summary.

Because Himbaechel chip databases are architecture inputs separate from the
executable, a `nextpnr-himbaechel` collection must pass exactly one explicit
`--chipdb PATH` (or `--chipdb=PATH`) and declare that file in `inputs`.
The collector then redirects the chipdb option to the same descriptor-backed
snapshot as other cohort inputs; implicit mutable installation chipdbs are
rejected before worker submission.

Before submission the collector locks the cohort name and constructs a basis
identity. After every worker has terminated and runtime/backend evidence has
been verified, it atomically publishes `cohort-<id>.json`. Its canonical
SHA-256 identity binds the cohort labels, command template, working directory,
frozen environment, source/runtime provenance, executable digest,
declared-input digests, artifact layout, required clocks, architecture, and
execution limits. Reusing a cohort ID with a
different bound identity fails closed. Each immutable run manifest embeds the
basis identity; each final result and the collection summary bind the published
execution-aware identity together with run-manifest and result-file hashes.

## Evaluation dataset

The dataset declares canonical required clocks as an array of explicit
`mapped_design_id`, `constraint_family`, and `clocks` records. Its
`cohort_identities` map contains the collector's canonical identity records;
every run supplies the matching `cohort_fingerprint_sha256`. The evaluator
recomputes each identity hash and requires its design, constraint family, and
clock list to match the run. A run cannot improve its classification by
omitting a failing clock.

Create a dataset from completed collection summaries with:

```
python3 python/seed_racing.py dataset collection-a.json collection-b.json \
    --output evaluation-dataset.json
```

The adapter reads each result or telemetry artifact once, verifies that exact
byte snapshot against its digest, retains the valid structured telemetry
prefix, and normalizes only iteration and repair metrics into the
evaluator's prefix allowlist. It never copies `run_end` legality or final
analogue timing into an observation. Those outcomes become visible at the
collector's full process duration, after terminal artifacts have been read.
The collector resolves its output root before planning and records absolute
artifact paths, including when `--output` was relative; the adapter rejects a
relative artifact path because it cannot be authenticated against the original
invocation directory. A candidate that never started because of total-budget
expiry or cancellation has no artifact to authenticate and is retained as an
unsuccessful zero-cost censored run with explicitly unavailable evidence.
An artifactless `runner_error` has neither authenticated lifecycle evidence nor
an honest measurable cost. The adapter records it in top-level `excluded_runs`
with `missing_artifact_and_cost_evidence` instead of discarding the cohort or
inventing a duration; excluded runs never enter policy replay or performance
claims.
An entirely unlaunched GPU cohort has no observed device identity; the dataset
accepts that absence only for never-launched runs that claim no backend. Any
launched GPU run still requires the cohort-bound exact backend and runtime
identity. An explicit `process_started: false` is accepted only with a
collector pre-launch terminal status; a contradictory completed, routing, or
timing result is rejected before it can become a success.
Combining collections is supported, but policy results spanning more than one
mapped-design/constraint family remain explicitly cross-design descriptive.

`python3 python/seed_racing.py evaluate DATASET --checkpoints 5,10 --quotas
8,2 --budget-seconds 600` replays random full-run and conservative
successive-halving schedules over fully observed traces. Checkpoints are fixed
elapsed times, not fractions of eventual duration. Only the allowlisted prefix
observation at or before a checkpoint is visible to ranking. A run's final
outcome becomes visible only at its explicit `outcome_observed_seconds`, not
from its eventual duration or final label. Numeric router
seeds and seed-bearing run IDs are excluded. The specified
`sha256-order-v1` scheduler ordering breaks ties and selects exploratory
survivors reproducibly across Python versions. Runs that finish by a checkpoint
leave the active ranking pool: terminal successes are retained immediately and
terminal failures are recorded but cannot consume a promotion slot.

The default `congestion-first-v1` ranking preserves the original lexicographic
score: least excess occupancy, least unrouted work, most recent progress, then
the table timing estimate. `--ranking-policy balanced-prefix-v1` is an explicit
offline alternative. Its ranked slots alternate between the best prefix table
WNS and the best congestion score, beginning with timing; already selected
candidates are skipped. This keeps timing and routability as separate prefix
objectives without fitting feature weights. Table timing remains only a ranking
proxy and never replaces final analogue setup-and-hold evidence. The random
exploratory survivor is selected independently from candidates not chosen by
either ranked lane.

When a cohort contains repeated executions of each seed label, use
`--replicate-stratified`. It evaluates each positive integer replicate index as
a separate race, so repeatability samples of one seed cannot consume multiple
promotion slots in the same simulated race. The supplied budget applies
independently to each stratum and the output labels that scope explicitly.
Within a stratum, duplicate `(cohort, mapped design, constraint family, seed)`
candidates are rejected using the collector's scalar-label identity.
Every stratum must also match the seeds and repeat indices sealed in the cohort
manifest. Equal observed strata are insufficient: if the same artifactless run
is excluded from every repeat, evaluation fails closed instead of silently
shrinking the declared population.
New collection identities seal `seeds` and `repeats` directly. A dataset made
by an older collector lacks those identity fields and therefore fails closed
unless each original declaration is supplied explicitly with
`--cohort-manifest MANIFEST`. The evaluator checks its cohort descriptor
against the dataset and records the declaration's canonical SHA-256 digest in
the replay; this is a compatibility path, not an inferred population.
Combined-run mode remains available for reproducing older descriptive results,
but it is not representative of a race that launches each seed once.

Each run's `outcome` separates `legal_route` from final analogue timing. It
must list every expected setup/related-clock constraint in `required_clocks` and
provide unique matching entries in `analogue_clocks`. Each required entry needs
finite `setup_wns_ns` and `hold_wns_ns` values and `available: true`. Missing,
duplicate, unavailable, or non-finite required results cannot become a success,
even when routing is legal or another clock passes. Constraint identifiers may
name related-clock checks; they need not be literal clock-net names.

Reports label ideal resumable replay as an optimistic simulation and report it
separately from restart execution, which charges every repeated prefix plus the
survivor's full rerun. The same fixed aggregate-compute budget bounds the
random, ideal-resumable, and restart policies; an unaffordable stage or final
run is censored. Aggregate serial compute remains distinct from any
future measured concurrent makespan. Synthetic results are diagnostics, not a
performance claim.

## Bounded full-run placement-policy screen

`python/seed_policy_portfolio.py` is an experiment-specific wrapper around the
same authenticated collector, not a general optimizer or early terminator.
See [the pre-result declaration](seed-policy-portfolio-plan.md) and
[the completed Atari screen](validation/seed-policy-portfolio-2026-10-07.md).
It varies criticality exponent only, freezes training selection before held-out
collection, and compares full-run serial schedules without prefix prediction.

```sh
python3 python/seed_policy_portfolio.py prepare --template ATARI_MANIFEST --output NEW_EVIDENCE_DIRECTORY
timeout --signal=INT --kill-after=30s 32400 python3 python/seed_policy_portfolio.py run --output NEW_EVIDENCE_DIRECTORY
python3 python/seed_policy_portfolio.py evaluate --output NEW_EVIDENCE_DIRECTORY
python3 mistral/tests/seed_policy_portfolio_test.py
```

`prepare` requires the retained Atari template and makes a fresh directory;
its fixed populations, policies, limits, order and hashes are declared before
PNR. Publish the canonical declaration digest before collecting. `run` admits
only declared cohorts under its retained deadline and free-space guard, locks
the operator, validates retained complete collections, and refuses incomplete
reruns. `evaluate` requires every declared attempt and the frozen training
selection. Use `--report NEW_FILE` to re-evaluate without overwriting the
original report. Raw artifacts stay outside git. Search budgets exclude
training/repeat costs, which must be added for first-use economics. No default
behavior, C++ routing logic, hardware programming or runtime contract changes.
