# Atari candidate-fanout reproduction gate, 2026-10-08

## Decision

Stop this follow-up after its two baseline reproductions. Both routes are legal
and miss final analogue timing, but neither reproduces the specific high-fanout
critical-path bottleneck selected from the older traces. No treatment, passing
control, replacement seed, default change or live termination was attempted.
This does **not** establish that a higher candidate fanout limit is ineffective:
the proposed intervention was not evaluated. It establishes that the historical
case selection did not transfer to the selected current compiler.

Close the seed-racing/portfolio campaign with existing defaults retained.
Keep its collector and telemetry for ordinary bottleneck investigations; further
work needs a fresh current-code hypothesis, not another broad seed sweep.

## Hypothesis and predeclared bounds

Retrospective analysis of all 104 authenticated [PR176 cases](seed-policy-multifamily-2026-10-08.md)
found that Atari candidate-only signoff transitions improved logged slack
13 times and preserved it 12 times, with no worsening. Reroute-containing
transitions improved seven times, worsened ten times and preserved it ten times.
These are rounded log observations, not causal comparisons; some reroute
transitions include candidate passes. Repair already exists and must not be
presented as a new mechanism.

In historical exponent-2 seed-34 and exponent-8 seed-35 failures, this net was
among the three largest routing-delay segments on the pixel-clock critical path:

```text
video.configured_MISTRAL_ALUT3_A_C_MISTRAL_ALUT5_A_Q_MISTRAL_ALUT4_D_Q_MISTRAL_ALUT5_E_Q
```

Each final report contained 65 distinct `(cell, port)` sinks on that net.
The existing candidate pass skips nets with more than 64 users by default,
including on the selected current main. This suggested testing the existing
`--gpu-opt analogueCandidateFanout=96` option before broader rerouting, not
raising the default without evidence.

The immutable plan and all eight manifests were frozen before new PNR. The
canonical plan digest was published in the task's shared note before launch:
`f4f6099a6496671afbb8c7c74c9b7ab17e2ecb425bd56f89be241bd219e7142e`.

The declared cases were:

| Role | Exponent | Seed | Options |
| --- | ---: | ---: | --- |
| Historical failure | 2 | 34 | Default, then fanout 96 |
| Historical failure | 8 | 35 | Default, then fanout 96 |
| Historical passing control | 2 | 35 | Default, then fanout 96 |
| Historical passing control | 8 | 34 | Default, then fanout 96 |

Admission order: two default failure reproductions, two default passing
controls, then the four treatments. Both reproductions must finish legally,
miss pixel setup while the other required clocks and all holds pass, and retain
the target net with more than 64 distinct sinks on a same-domain pixel critical
path. Failure of either reproduction stops admission before controls/treatments.
Controls must pass before treatment admission. No cases may be replaced.

Maximum eight invocations, serial on the local RTX 3090; 600-second per-process
timeout, original persisted 5400-second campaign deadline and 12 GiB free-space
guard. The maximum admitted PNR time was 4800 seconds; preparation, build,
freezing and analysis costs are separate, not assumed free.

## Selected source and inputs

Clean native source at build time:
`e35e008864792f640f3776f99e11e3d054c288df`, then-current main. A fresh isolated
CUDA Release/SM86 build used the retained clean Mistral dependency
`7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039`; it did not overwrite the archived
binary. New binary SHA-256:
`28ff53c7ca591057f3d6bcd2c283fb58a95094eefe8117f7df8d06a4ec604bc2`.

The older 104-case campaign used native source `9643627f`, not this compiler.
Current main already has cooperative analogue repair budgeting and an enforced
setup-slack calculation; old timeout and slack observations cannot be treated
as current defects without reproduction.

Use the exact retained Atari mapped input and constraints; no resynthesis:

| Input | SHA-256 |
| --- | --- |
| Mapped JSON | `16e052117019b375794ace17a854a75db7aa96ab81cfe31fa7dcf7b52e49a506` |
| QSF | `79159cd0618cfc90980897462e7fc9f17349d4484291f932b1bec1fdacb3185e` |
| SDC | `a0f6cf1636d4bf91643477d923761872edddeb64f70b9432298b1cc4c88f14bc` |

Device `5CSEBA6U23I7`, heap timing weight 2000, requested frequency 74.25 MHz,
CUDA device 0, `CUDA_CACHE_DISABLE=1`, ordinary repair options and
`--timing-allow-fail` were fixed. Required clocks:
`pixel_clk`, `system_clock.clocks[0]`, `system_clock.clocks[1]`.
The existing collector sealed binary, inputs and dependency closure and retained
raw logs, telemetry, timing reports and RBFs. Runtime/backend identities matched
between the two new cases; comparison to the old campaign is not a controlled
compiler-change or runtime-change attribution.

## Actual results

| Case | Process cost | Pixel setup | System[0] setup | System[1] setup | Outcome |
| --- | ---: | ---: | ---: | ---: | --- |
| Exponent 2, seed 34 | 397.239 s | -1.455 ns | -0.596 ns | +76.618 ns | Legal; analogue failure |
| Exponent 8, seed 35 | 316.293 s | -2.768 ns | +1.912 ns | +76.102 ns | Legal; analogue failure |

All three required hold margins are positive in both cases. Both complete
top-router telemetry records report legal routing and a passing table gate;
those facts remain separate from the final analogue failure. Normal process
exit under `--timing-allow-fail` does not supply a successful timing label.

The first final pixel path ends at `video.lookup_valid_MISTRAL_FF_Q` and the
second at `video.line_available_MISTRAL_FF_Q`. Neither representative same-domain
pixel path contains the selected configuration net. That net still has 65
distinct reported sinks in the first report, but its fanout is not the selected
worst-path bottleneck. The first case additionally misses system-clock setup.
Both reproduction gates are false. The six other declared invocations were
never started and are not inferred failures, timeouts or successful controls.

Total observed process cost: **713.531316108 seconds (11.892 minutes)**.
No restart or repeated prefix was discounted. Build/analysis cost was not
profiled as part of that total, and no end-to-end savings claim is made.
All observations were retained; no timeout, censoring or exclusion occurred
among the two started attempts. Case selection was retrospective, and two
observations do not establish broad generalization or same-seed nondeterminism.

## Validation and retained evidence

Fresh CUDA compiler build completed. Six synthetic admission-gate tests passed,
including cross-domain exclusion, duplicate-sink counting, controls on other
clock/hold failures, censored outcomes and the strict positive-margin gate.
The dry-run verified the eight-invocation ceiling. Reauthentication uses existing
dataset validation, exact declared input/binary/argv hashes and matched runtime
identities. Both complete collections and the stop decision are immutable.

Evidence root in the enclosing FES worktree:
`out/dev/analogue-candidate-fanout/evidence`.
The [diagnostic runner](../../mistral/tests/gpurouter/analogue_fanout_screen.py)
and [six-test fixture](../../mistral/tests/gpurouter/analogue_fanout_screen_test.py)
are retained here; the task also retains the earlier five-test read-only trace
analyser in attachments. This is a pinned diagnostic campaign, not a new
general-purpose production scheduler. Preparation deliberately requires a clean
checkout of the selected source revision and a built binary in its sibling
`build-mistral-cuda` directory; preserve the script separately when checking out
that earlier revision. The input template comes from the retained Atari cohort.

```sh
python3 mistral/tests/gpurouter/analogue_fanout_screen_test.py
python3 /path/to/analogue_fanout_screen.py prepare --repo /path/to/selected/nextpnr \
  --template /path/to/retained/atari/critexp-2.json --output /path/to/new/evidence
python3 /path/to/analogue_fanout_screen.py dry-run --repo /path/to/selected/nextpnr \
  --output /path/to/new/evidence
python3 /path/to/analogue_fanout_screen.py run --repo /path/to/selected/nextpnr \
  --output /path/to/new/evidence
```

These commands reproduce the declared protocol, not an instruction to spend a
new budget after the negative gate. Existing complete collections are reused
only after authentication; incomplete prior collections fail closed. A recorded
stop decision admits no further processes.

| Artifact | File-byte SHA-256 |
| --- | --- |
| Plan | `db92390984d87dd37ebd0d5c8ec350bed2670cc3ae1b3d7475a8202c373788a6` |
| Stop decision | `50b7379a0109065386717341ab1854fe9202799eea191b4f2bcf986a22ea1391` |
| Exponent-2/seed-34 collection | `4065b530c65cec3926b8a443fed1a655a7add125f5f5637419cbfe8b88d80cba` |
| Exponent-8/seed-35 collection | `3c25a7b8e16854154ba267b8d3ab03571043c74d4b70731216c93c9e90857926` |

## Previously reviewed finalization fix

PR176 merged head `c026e003`, not the subsequently pushed `0ff9398a` fix.
This delivery carries that Python-only restart-safe finalization change forward:
recompute/authenticate before reusing byte-identical evaluation evidence;
validate and preserve an existing completion marker. Explicit evaluation still
requires a fresh report file. All 133 repository tests pass (23 multifamily,
21 portfolio, 89 foundation), including the six finalization regressions.

No native algorithm, default, FES module, toolchain lock, firmware, constraint
or shared contract changed. No FPGA programming or hardware acceptance occurred.
