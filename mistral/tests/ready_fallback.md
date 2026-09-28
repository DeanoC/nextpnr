# Final ready-net fallback candidate diagnostic

`NEXTPNR_MISTRAL_READY_FALLBACK=<prefix>` enables a bounded experiment after the
final bitstream configuration and before signoff. Absent/empty values return
before identifier allocation. This fixture requires the corrected generic
replication baseline: one `cmd_ready_1` sink, five downstream LUTs, and exactly
224 endpoints (223 FF ENA and one SCLR), on one rising memory clock domain.

The pass asks `GpuCandidateRouter` for at most eight complete alternative trees,
using the current calibration and prior (1.25), while all unrelated routing is
reserved. It does not change the graph, placement, pin mapping or delay model.
Each available tree is configured from scratch with `configure_bitstream(false)`
and evaluated by fresh full STA with clock skew. The target's analogue cache
must remain failed, so timing uses the same scalar fallback as normal signoff.
The observation map, type calibration and prior never change. Every unrelated
cache entry, net route and placement is checked against the starting context.

A qualifying candidate must preserve all 224 setup slacks, improve their worst
setup slack by at least 20 ps, keep endpoint hold slack at least `min(old, 0)`,
and preserve every clock's achieved Fmax. Positive hold margin can decrease;
new or worsened hold violations cannot be accepted. Exactly one timed domain
pair per endpoint is required; its clock identities and edges are checked.
The best worst-endpoint setup slack wins, with the earliest generated candidate
breaking ties. The original route is retained if no candidate qualifies.
Every trial restores and reconfigures the original tree before the next trial.
The selected result is configured and checked again before signoff and output.

Evidence includes before/after JSON with pin sidecars, before/after RBF bytes,
complete ready timing/model traces, every generated tree, per-trial endpoint and
clock guards, selection rows and an audit record. ROUTING attributes are
refreshed after the final selection so normal `--write` reflects the new tree.
Candidate-router scratch net indexes and inserted settings defaults are restored.

Tests cover guard rejection cases, disabled identifier neutrality, real failed
HPS fallback reevaluation with no observation learning, and CPU candidate
reservation on a small synthetic graph. Synthetic tests are not full-design
physical acceptance. The fresh full flow must establish exact baseline bytes,
complete independent route/state proofs, legality and final timing. Imported
routed JSON is not a substitute: scaffold loading locks strengths and does not
reconstruct the complete calibrated model and clock context.


The final diagnostic compares the complete per-cell/BEL placement-check vector
against its routed baseline after every trial, restore and final selection.
It does not require all placement checks to be true after routing: LAB route-through
insertion rewires FF.DATAIN while leaving the earlier ffInfo.datain
cache untouched. A real backend fixture demonstrates this phase difference and
isolates the cause with a test-only cache refresh. The diagnostic does not refresh
live placement caches or change packing. Before/final placement-validity TSVs
record every bound cell, its result and available component failure reasons;
the audit records equal invalid counts. This census is separate from target
route availability/connectivity and exact graph/pin/BEL preservation checks.
Trial and clock rows are flushed immediately so an aborted run retains evidence.
