# Read-only HPS-ready signoff trace

`NEXTPNR_MISTRAL_READY_TRACE=<prefix>` enables a diagnostic after final bitstream
configuration, signoff timing analysis and the existing optional arc dump. An
absent or empty prefix returns before identifiers, files or state are touched.
It covers every connected FPGA2SDRAM `cmd_ready_*` output and logical sink.
The current fixtures have one physical wire per sink; an unexpected multiwire
mapping fails explicitly rather than silently selecting a wire.

The optional `AnalogueTrace` argument observes existing analogue control flow.
It records the failure reason and hop, mode, source/sink pip wires, rise/fall
input-wave sample counts, and whether each hop completed. Generated, P2P and
NO_DELAY zero-delay hops are distinguished from an unfinished empty-input-wave
hop and later unattempted hops. Existing return values, assertions, numerical
calculations, routing and calibration are unchanged. Failed jobs can contribute valid completed prefixes to observations. Unfinished
hops are excluded; completed generated, P2P and NO_DELAY zero observations remain
valid. The state manifest explicitly records this completed-only contract.

Outputs:

- `.arcs.tsv`: source/sink identity and wires, route completeness, cache and
  recomputation status, failure location, selected override, Context scalar and
  quad delays, reconstructed fallback sums, source-wire delay, and cached and
  recomputed analogue quads, and the reconstructed Context quad.
- `.hops.tsv`: full source-to-sink pip paths, static and actual pip delays,
  destination-wire delays, observed values, exact provenance branch and
  per-hop analogue progress. Generated sources use type_id=-1.
- `.observed.tsv`: complete final observed-pip map, including zero observations,
  stable source/destination names and static table components.
- `.types.tsv`: all 256 type-calibration totals and hop counts.
- `.state.json`: calibration/cache flags, exact binary32 prior bits plus decimal
  and hexadecimal forms, coverage counts and read-only invariant results.

Quad components are rise_min, rise_max, fall_min and fall_max in integer ps.
Fallback scalar sums per-hop maxima; the raw fallback quad sums corresponding
components. The Context quad can differ: a failed cached override writes its
cached quad into the initial accumulator before returning false, and Context
then merges fallback minima/maxima into that seed. A cached zero therefore
clamps positive fallback minima to zero. The `override_*` columns retain the
post-call accumulator seed and `context_reconstructed_*` records the exact
existing merge. This diagnostic intentionally preserves that API behavior.
They are reconstructed independently. Successful analogue overrides are checked
against their selected cached/recomputed value, not incorrectly compared with a
fallback sum. Analogue per-hop integer truncations need not sum to the final
analogue delay, which truncates the floating-point total once.

The exporter checks every observed value, type total, cache entry, route binding
and strength, placement and strength, model flag/prior and the full context
checksum before/after. Tests exercise the real GIN.51.64.18→H6.46.64.32 empty-wave
failure, traced/untraced equivalence, an exporter fixture with a zero observation,
and every fallback provenance branch. That small fixture does not run bitgen;
the full fresh flow provides authoritative final cache/model provenance.
