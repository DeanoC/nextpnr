# RAM-test reconstructed research baseline

Selected 10 October 2026 for future historical-profile comparisons supporting
[FES issue 264](https://github.com/DeanoC/fes/issues/264).
Baseline ID: `ramtest-reconstructed-114mhz-seed2-20261010`.

The selected uninstrumented full-flow result is **114.324913 MHz memory**, with
93.231400 MHz pixel and 378.582397 MHz capture. The compiler-matched unstacked
control reaches 106.157112 MHz memory: the reconstructed family stack gains
8.167801 MHz. This baseline preserves the completed reference for further work;
it does not select a new production compiler or change default recipes.

| Comparison | Memory MHz | Pixel MHz | Capture MHz |
| --- | ---: | ---: | ---: |
| Same compiler, unstacked control | 106.157112 | 94.500099 | 347.793488 |
| Selected reconstructed family stack | 114.324913 | 93.231400 | 378.582397 |
| Instrumented no-driver control, separate diagnostic | 115.366867 | 94.625282 | 300.242950 |

The last row contains a HIP trace preload. There is no matching instrumented
with-driver control, so this comparison does not isolate driver-copy benefit.
Three earlier fresh full flows failed at HIP initialization on both GPUs;
bounded traced uploads and the complete traced control subsequently succeeded.
The initialization failure's cause remains unknown.

## Fixed profile and provenance

The [baseline manifest](../../validation/ramtest-reconstructed-baseline-2026-10-10.json)
contains exact commands, environment, source/dependency/build identities, input
and selection-guide hashes, GPU identity, final artifact hashes and results.
The [reconstruction findings](historical-117-reconstruction.md) and
[measurement record](../../validation/mistral-historical-117-reconstruction-2026-10-09.json)
retain the other compiler lanes and qualification limits.

The reference uses published historical source
`ae5350d7b3ea1a14bd10fefbcc0d7f272a331eb6`, Mistral dependency
`7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039`, reconstructed synthesis SHA256
`f90717c8f06708485a125aad1488531a45f9f92e725bad0a969c8a5ddafe0b65`,
BUILD_ID `7168b508ab424b70f1c0f87b2035d821`, seed 2 and device
`5CSEBA6U23I7`. It runs on the RX 7900 XTX, `gfx1100`, unique ID
`0x63e81b58a39ea1f3`. The rebuilt executable hash is
`a05b1e190d25543c36e849ab5762cec3864753911762da698e9f44433ba10ba7`.

The profile combines pre-placement reduction balancing, three local remaps
(groups 8/1/1), a qualified internal comb cut, capture locality, two placed
reductions, decomposition and LUT-driver copying. HeAP timing weight/exponent
are 10/2 and enable replication budget is 4. The exact roots, candidate choices,
guide dependencies and environment are preserved in the manifest. All recorded
pass families qualified, with fresh candidate identities. Capture locality
performed 43 moves versus the historical record's 60.

## Replay and comparison rules

The manifest's absolute paths locate retained local evidence. Large synthesis,
guide, timing and RBF artifacts are not bundled in this PR. Verify their hashes
before replay; missing artifacts are a blocker for this exact profile. Rewrite
output destinations and `TMPDIR` to a fresh main-disk directory. Do not execute
the recorded command directly into retained outputs. Verify which free physical
GPU an ordinal identifies, clear inherited experimental settings and restore
only the recorded explicit environment.

Remapping requires fresh packing and placement with native guide qualification.
The partial compatible-path guide selects candidates; it is not signoff evidence.
For a new experiment, compare against a matching baseline with the same compiler,
dependencies, RTL/input, constraints, profile, GPU and seed. Report final timing
for every required clock pair, setup and hold coverage, legality and artifact
identity. Extend useful experiments to paired seeds 1/2/3. Changes to mapping or
placement remain full-flow comparisons; router-only comparisons additionally
require validated identical placed graphs and timing-state replay.

Current-main runs use a different compiler and model and must keep their own
control. The documented historical 117.343346 MHz result remains 3.018433 MHz
above this selected reference; its original synthesis and executable are missing.
Matching a headline Fmax does not authenticate that historical run.

## Acceptance and validation

The unchanged memory constraint is 130.005203 MHz. The reference's worst reported
same-edge memory margin is **−1.055 ns**, with 6.757 ns routing delay on an HPS
`f2sdram.cmd_port_clk_1` to port1 `skid_address[28]` FF-enable path. Six other
reported setup-pair minima are positive. This historical compiler exports no
hold aggregate or hold paths; complete external-interface coverage, uncertainty
reserve, physical calibration and hardware acceptance are not established.

The rebuilt historical source has 170 individually passing native tests and one
optional preflight skip, plus seven passing CLI checks. The bulk test process
was terminated after 160 passes at approximately 37 GB RSS; this is not a bulk
suite pass. The baseline and unstacked control each completed native routing,
final analogue timing and RBF generation. Neither closes 130 MHz nor supplies
hardware acceptance. This documentation change adds no executable behavior.
