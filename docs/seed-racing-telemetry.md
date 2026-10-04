# GPU-router telemetry JSONL schema

`--gpu-telemetry PATH` enables schema-version-1 telemetry for `--router gpu`.
The option is off by default. `PATH` is created exclusively: an existing file,
including an input or another run's output, is never overwritten. An open or
write error fails the opted-in run instead of silently returning incomplete
evidence.

Every line is one finite JSON object. Common keys are `schema_version`,
`sequence`, `run_id`, `event`, `phase`, `attempt`, `elapsed_s`, and
`elapsed_unit`. Sequence numbers start at zero and increase within a file.
Negotiation attempts are numbered separately, so re-negotiation after repair
does not masquerade as continuation of the initial loop. Missing metrics are
`null` and carry a sibling `<name>_unavailable_reason`.

`run_start.seed` is the original explicit `--seed` value, or the generated
value from `--randomize-seed`, encoded as a decimal string so every `uint64_t`
value remains exact. It is null with reason `not_explicit` when neither option
was used. It is not the RNG's warmed or subsequently advanced internal state.

The initial events are `run_start`, setup `phase_start`/`phase_end`, and a
negotiation event stream. Each negotiation iteration records occupancy,
excess occupancy, table-model timing (when enabled), work counts, batching,
and elapsed time. Timing repair emits `repair_round` events and its own phase
boundaries. `run_end` reports router1 legality separately from
`analogue_timing_pass`, which is null with reason `downstream_phase` for
Mistral and `not_available_for_architecture` elsewhere. Mistral's analogue
multi-clock signoff and repair happen after the common GPU router
returns and remain authoritative. The offline evaluator consumes a separately
normalized final outcome without converting one Fmax or table WNS into
analogue success. Downstream Mistral analogue candidate searches and full
re-route helpers do not reopen the top-level router's exclusive telemetry path.

Telemetry only observes existing state. It owns no router state, calls no RNG,
does not run extra timing analysis, flushes each bounded record, and retains no
unbounded in-memory trace. A missing `run_end` or a truncated final line is an
incomplete trace; the collector preserves the raw exit code/signal and
classifies timeout, cancellation, crash, routing failure, analogue timing
failure, and success independently.
The collector requires a valid terminal record for routing classification and
checks every manifest `required_clocks` entry in the final report. Process
exit zero alone is never a successful outcome.

The bounded collector and evaluator contract is documented in
[seed-racing-tools.md](seed-racing-tools.md).
