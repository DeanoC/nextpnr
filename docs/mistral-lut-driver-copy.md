# Guarded placed LUT driver copying

The opt-in Mistral pass copies an ordinary LUT near one critical arithmetic
operand. It preserves the original LUT and its other consumers, reconnects
only the selected operand, and adds the copy's input loads. It uses timing,
connectivity and legal device geometry; production code contains no RAM-test
cell names or locations. A copy adds no register or pipeline cycle.

`--remap-lut-driver-critical REPORT` lists qualified candidates from actual
reported LUT-to-arithmetic paths. `--remap-lut-driver-candidate N` applies one
freshly qualified candidate. Candidate ordinals belong to the particular
compiler, graph and profile that produced the listing. Full routing and final
signoff remain necessary after placement qualification. The pass is disabled
unless explicitly requested and rejects FES scaffold/cart or partial flows.

The search accepts defined ALUT2 through ALUT6 tables, ordinary scalar
arithmetic operands, and isolated legal copy sites. It is bounded to 16 edges,
24 shortlisted sites and 16 timing probes per edge, with a 250 ps predicted
improvement requirement. Existing protected cells, original placements,
dedicated carry successors, native input identities and transactional rollback
remain guarded.

Qualification checks every affected endpoint's launch/capture clock pairs.
Timed pairs retain their real setup window, nonregressing setup/path maxima
and related hold checks. Unrelated pairs have no invented setup window: their
path maxima may not increase and their minima may not decrease in either the
normal clock-skew frame or a separate reference-free frame. Loops, unknown
clocks, incomplete tags and nonfinite bounds reject the candidate. This
includes consumers of the original output and the newly loaded input sources.
Global clock and hold guards also remain required.

`--timing-report-paths N` supplies additional actual registered setup endpoints
per reported clock pair, including its original critical path. The range is
1 through 16,384; the default remains one. Increasing report coverage changes
neither the legacy worst-path nor Fmax/hold results. An explicitly requested
`--report --no-route` obtains fresh predicted timing for the final placed graph;
it does not claim routed analogue signoff. Preflight rejection diagnostics
identify why an edge cannot be probed.

## RAM-test measurement

The completed full HIP GPU route reports memory 117.34334564208984 MHz, pixel
93.4928970336914 MHz and capture 378.5823974609375 MHz. Its accepted reference
reports 116.64527893066406, 93.3096923828125 and 307.6318054199219 MHz. The
memory gain is 0.6980667114257812 MHz, and all three reported Fmax values
improve. Memory still misses its 130.0052032470703 MHz constraint.

The current report contains 15,290 setup endpoint paths across seven actual
clock pairs. All required non-target windows have positive headroom, including
the 5 ns capture-to-memory and 3.846 ns falling-to-rising memory windows. The
last signoff reports zero hold violations. These are reported timing results,
not hardware acceptance or an independent physical delay calibration.

The measured source is `5dd69a8ddece6280d576816d2277c73f80607f52`; the CLI
binary is SHA256 `2ebe29caf7ca46b3c68afd23d26efda1426c94bc6f1b214d22f810bb5c48351c`.
The publication retains those compiler and test bytes and adds documentation.
The exact profile stacks the earlier capture locality, placed reductions and
control remaps before driver copying. It retains the fixed RTL/BUILD_ID,
constraints, seed 2, device, placement options and GPU device 1. No FES default
recipe, compiler pin, ABI or hardware deployment is changed by this PR.

The strict initial full comparison remains preserved as a rejection: the new
RBF is uncompressed, and inserting the copy reassigns private route-through
output export numbers. The same 5,232 original buffers retain their names,
input sources, unique FF DATAIN consumers, placements, pin images and masks.
Those document-local numbers require an explicit per-buffer identity ledger;
a numeric offset or global buffer collapse is insufficient. The separately
reviewed second comparison passes that ledger and the independent native and
physical checks, as well as the practical and all-clock timing gates. It is
SHA256 `a4a91235f652af541265daa5a3b6e12e658dc6f01089ce16ecad622b95b2b304`
and binds 526 artifact hashes and 281 execution-file hashes. It evaluates all
5,232 buffer banks and 4,158 shared ALMs, along with the selected copy,
arithmetic operand and immediate carry successor. The first rejection remains
unchanged. No buffer was removed, added or collapsed.

The Mistral build and 31 focused backend/CLI cases pass, including mixed clock
pairs, actual phase windows, unrelated extrema and clock-skew regressions.
The common timing changes also compile for the generic architecture; generic
runtime STA was not exercised. The earlier 37-case focused run is retained
separately. Cold compilation retains existing warnings; one fixture rebuild
also reports a dangling-else warning in the test code.

The [machine-readable validation record](validation/ramtest-lut-driver-copy-2026-10-01.json)
binds the completed comparison, invocation, input identities and raw artifacts.
The full route completed normally in 733.199693730101 seconds. Raw reports and guide inputs
remain retained locally; the summary does not bundle them. Exported pin/mask
and connectivity checks do not independently decode RBF/CRAM or reconstruct
unexported indexed-store, cache or GPU state.
