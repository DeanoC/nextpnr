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
exclusively, captures stdout/stderr, hashes available artifacts and the resolved
binary, preserves exit codes/signals/timeouts, and terminates only the fresh
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
GPU-capable Mistral and generic nextpnr collections must bind exactly
one explicit `--router`; `gpu` additionally requires exactly one
`--gpu-telemetry {telemetry}` binding. These bindings must occur before
`--`. JSON/read inputs whose settings override the declared router are rejected.
Explicit `router1`/`router2` CPU collections
do not require GPU backend attestation. HIP runtimes older than version 6 remain
usable for ordinary GPU routing, but their telemetry is explicitly unattested
because those runtimes do not expose the exact device UUID; seed-racing
collection rejects that backend identity.

The command executable is resolved and copied once into a sealed descriptor
before workers are submitted. Every run executes that descriptor and records
its original path, display snapshot path, descriptor launch path, and SHA-256.
On Linux nextpnr installations, an executable-relative share tree is copied into
the frozen cohort runtime, made read-only, content-bound in runtime evidence,
watched recursively for any write/attribute/name mutation (including a later
restore), re-enumerated after all workers, and selected through an inherited
directory descriptor in `NEXTPNR_EXECUTABLE_DIR`; replacing its display path
cannot redirect later workers. Collection fails closed without Linux
sealed descriptors, pidfds, and `/proc`. The executable's dynamic dependency
closure is copied and content-checked into individually sealed descriptors before
submission. Each worker is invoked by the sealed dynamic-loader descriptor with
the sealed library descriptors explicitly preloaded, so later pathname swaps
cannot alter the bytes consumed by another worker. The command must be a native
`nextpnr` ELF executable; shebang commands, standalone language interpreters,
generic launchers, and nextpnr Python hooks are rejected because their implicit
module/resource search trees cannot be bounded.
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
