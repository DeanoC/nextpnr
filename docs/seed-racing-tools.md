# Seed-racing collection and offline replay

`python/seed_racing.py` is a standard-library-only foundation for bounded
collection and shadow evaluation. It does not stop a live PNR run or provide a
checkpoint/resume mechanism.

## Collection manifest

Run `python3 python/seed_racing.py collect MANIFEST --output DIRECTORY` or add
`--dry-run` to inspect the expanded argv without creating output. The JSON
manifest has schema version 1 and contains:

- `cohort`: `id`, `design_id`, `mapped_design_id`, and `constraint_family`;
- `command`: an argv string array, never a shell command;
- `seeds`, `repeats`, and `limits` (`per_run_seconds`, `total_seconds`, and
  `concurrency`);
- `inputs`: exact mapped netlist, constraints, included constraints, memory
  initialization, or other inputs to hash;
- `required_clocks`: every clock whose final timing constraint must pass,
  using the exact final-report clock names;
- `artifacts`: names mapped to paths relative to each unique run directory;
- an explicit environment allowlist and provenance object. Children receive
  only that map plus the recorded platform-minimum environment (`PATH`,
  deterministic `LC_ALL=C` on POSIX, and the standard process-creation
  variables on Windows); ambient GPU visibility, thread-count, loader, and
  routing controls are not inherited.

Command arguments may use `{seed}`, `{repeat}`, `{run_id}`, `{run_dir}`,
`{telemetry}`, and `{report}`. The collector creates every run directory
exclusively, captures stdout/stderr, hashes available artifacts and the resolved
binary, preserves exit codes/signals/timeouts, and terminates only the fresh
child process group it owns. Cancellation gates future launches before it
terminates active groups, so queued jobs cannot start after interruption. The
collector writes the complete terminal summary before propagating an interrupt.
Declared cohort inputs are copied once into read-only, uniquely named snapshots
before submission. Exact input-path argv entries and `--option=PATH` values are
rewritten to those snapshots, and every run records the snapshot path and hash,
so an external producer cannot replace bytes between a hash check and the
child's open. A missing input or output is recorded as unavailable; it is not
silently substituted.

The command executable is resolved and copied once to a read-only cohort
snapshot before workers are submitted. Every run launches that same snapshot
and records its original path, snapshot path, and SHA-256. The collector keeps
the raw process lifecycle in `process_status`, then classifies the run from
the `telemetry` and `final_report` artifacts. A valid terminal `run_end`
determines routing legality. Every declared required clock must have finite
final report evidence; a failing clock is `analogue_timing_failure`, an
illegal route is `routing_failure`, and missing or truncated evidence is
`incomplete_evidence`. A standard nextpnr `fmax` report can establish a
setup failure, but cannot establish success because it lacks hold evidence; a
normalized `outcome.analogue_clocks` report supplies both setup and hold WNS.

## Evaluation dataset

`python3 python/seed_racing.py evaluate DATASET --checkpoints 5,10 --quotas
8,2 --budget-seconds 600` replays random full-run and conservative
successive-halving schedules over fully observed traces. Checkpoints are fixed
elapsed times, not fractions of eventual duration. Only the allowlisted prefix
observation at or before a checkpoint is visible to ranking. Numeric router
seeds and seed-bearing run IDs are excluded; an independent scheduler RNG
breaks ties and selects exploratory survivors. Runs that finish by a checkpoint
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
survivor's full rerun. Aggregate serial compute remains distinct from any
future measured concurrent makespan. Synthetic results are diagnostics, not a
performance claim.
