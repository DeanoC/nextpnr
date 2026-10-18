# Mistral LUT placement policy, issue #206

Making the isolated LUT pin estimate unconditional in
[PR #205](https://github.com/DeanoC/nextpnr/pull/205) changes placement
criticalities and routing. It is a QoR heuristic, even though it matches the
input allocation of an isolated cell. It does not establish the final mapping
of a pair of LUTs sharing an ALM.

[Issue #206](https://github.com/DeanoC/nextpnr/issues/206) reports two regressions
against `990a0940`: ST shell seed 6 loses pixel frequency, and Coleco shell seed
4 loses its system-clock pass. The saved ST critical pixel paths have 8.702 ns
of routing at the parent and 10.525 ns at `1fe73196`; logic delay falls from
5.901 to 5.042 ns. The path and placement change, rather than merely the timing
reported for identical hardware.

The default therefore retains the parent's fastest-first placement policy.
(#214 later legalised the last L5 input onto F and made that isolated map the
default estimate; this record describes the #205/#206 policy only.)
`--isolated-lut-placement` explicitly selects the isolated estimate without
permuting private LUT pins. `--arrival-pin-assignment` retains its existing
combination of isolated estimates and arrival-based private pin assignment.
Neither option is enabled by saved JSON provenance. Both are initialized in
`createContext`, including Python `--run` invocations without JSON input.
The isolated estimate also handles hypothetical cells without nets and skips
disconnected inputs when other inputs are connected.

This is a mitigation of the default-policy regression. It does not make the
isolated heuristic dominate the parent across seeds, retain its gains on other
seeds, or close the older ST shell timing failure in #202. Final timing still
uses legalised physical pins under either placement policy.

## Replay identity

Fresh candidate runs use the issue's inputs on Powerboat at
`/home/deano/tmp/n203-05d0/in/{st-shell,coleco-shell}` and the same GPU 0,
RX 7900 XTX, HIP. The parent and PR comparisons below are the retained issue
runs from `/home/deano/tmp/n203-05d0/run205`, not newly rerun parent builds.
The GPU is shared with other work; wall times are not performance evidence.

The candidate is based on `990a0940`, with the source diff retained in
`build/issue206/source.patch`. The replay binary is
`build/issue206/nextpnr-mistral-206`, SHA256
`7c7d3ea589f24f9b1baf1079df1f38343b29e1e32f76a7021a8cd5d205386187`.
It is a GCC 14.2.0 Release build with IPO, `ARCH=mistral`,
`GPU_ROUTER=HIP`, `gfx1100;gfx1201`, Python/GUI/OpenMP disabled, using
Mistral `8fcc4cb41c51f8918f1d3ad70def2febcbf20d8f` from toolchain slot
`1015e4eb9e191cd69f1c3b87fd99f83f0560f6ea7a97509d9f0c4fe45f97e7b4`.

Input SHA256:

| Input | SHA256 |
| --- | --- |
| ST `synth.json` | `7ebed3771416a0a015e865653b46a45aaea9e776c090ef69793802ecc96ec55f` |
| ST `socket.qsf` | `461004549976f546503868b5a3e2d8968de9ed36638f75d2c6b93d53cd4a514c` |
| Coleco `synth.json` | `fd96c23d1c9ee1b717605621b5526d6173420d1e7882dcba88b07c1345540392` |
| Coleco `socket.qsf` | `35f48121aa1559c2cd2524c4c0d525d2e92eb90d445b81837b62b438dfd87155` |
| Both `clocks.sdc` | `a0f6cf1636d4bf91643477d923761872edddeb64f70b9432298b1cc4c88f14bc` |

Each default replay uses the original command, substituting the matching
input directory and seed (Coleco 4, ST 6):

```sh
nextpnr-mistral --json synth.json --device 5CSEBA6U23I7 \
  --qsf socket.qsf --sdc clocks.sdc --freq 74.25 --seed N \
  --placer-heap-timingweight 2000 --placer-heap-critexp 5 \
  --router gpu --gpu-device 0 --timing-allow-fail --report timing.json
```

The isolated-only Coleco trial additionally passes `--isolated-lut-placement`.
Logs, exact commands, complete reports, hashes, and comparison results are
retained under `build/issue206`. These commands deliberately match the issue's
report mode, without `--rbf`; `timing_summary.final_analogue_model` is false.
They do not qualify a final bitstream or establish hardware acceptance.

## Results

Both default replays completed successfully. Their complete report files are
byte-identical to the retained parent reports, and every logged route checksum
matches the parent. The default therefore removes both #205 regressions on the
exact issue cases. The older ST pixel and system timing failures remain.

All values below are MHz from the final routed delay-table report:

| Design / seed | Policy / evidence | Pixel | System | Audio |
| --- | --- | --- | --- | --- |
| st-shell s6 | parent (saved) | 66.04 FAIL | 42.86 FAIL | 139.30 PASS |
| st-shell s6 | #205 (saved) | 61.69 FAIL | 44.86 FAIL | 215.52 PASS |
| st-shell s6 | default (fresh) | 66.04 FAIL | 42.86 FAIL | 139.30 PASS |
| coleco-shell s4 | parent (saved) | 473.93 PASS | 53.54 PASS | 179.40 PASS |
| coleco-shell s4 | #205 (saved) | 505.05 PASS | 52.21 FAIL | 185.36 PASS |
| coleco-shell s4 | default (fresh) | 473.93 PASS | 53.54 PASS | 179.40 PASS |
| coleco-shell s4 | isolated option (fresh) | 505.05 PASS | 52.21 FAIL | 185.36 PASS |

The fresh isolated-option Coleco report is byte-identical to the retained
#205 report, confirming that the option selects the isolated policy independently
of arrival-based pin permutation. That policy remains a failing choice for this
Coleco seed; it is available for explicitly qualified profiles.

Default report SHA256:

- coleco-shell s4: `e6936b93e401500fbe71884327f80ce3ad20062614772d6372ecfc885e1faeb7`
- st-shell s6: `dbb5fdab6b9c7a15d9a0fc0d893d49235e619f54487be11f7c8baf18007ed3dc`

The report comparisons include every reported critical path, clock Fmax, setup/hold
summary, slack histogram, and utilization entry. `build/issue206/results.json`
records the results and route-checksum comparisons. Neither complete shell was
rebuilt with `--rbf`, and these results do not constitute bitstream signoff or
hardware acceptance.

## Tests

A second Release HIP build enabled Python, with CLI binary SHA256
`af7321e04c5251db3e065b19534e3301d6b549b2ebe694c099b27aa49c104cdf`.
The final focused architecture run passed all 26 tests in these suites:
`LutPlacementTiming`, `LutInputArrival`, `LabPinmapTest`,
`PlacedReductionDiagnostic`, `PlacedReductionTest`, `TwelveReductionTest`,
`NarrowSearchTest`, and `PlacedReductionProbePolicy`.
The tests cover default pin scoring, isolated estimates on both halves,
hypothetical and sparse LUTs, legal physical timing, shared/inverted/private
pins, locked scaffolds, and reduction admission/rollback behavior.

CTest passed all three selected entries:

- `nextpnr-mistral-arrival-pin-assignment-cli`: ten tests passed, none skipped.
  This includes both flags independently, their combination, saved-provenance
  stripping, and Python `--run` without JSON.
- `nextpnr-mistral-physical-lut-timing`: all five logical permutations retained
  identical RBFs and endpoint setup/hold timing.
- `nextpnr-mistral-physical-lut-timing-isolated`: the same five replay checks,
  starting from an isolated-policy route and replaying without that option.

```sh
build/nextpnr-mistral-test \
  '--gtest_filter=LutPlacementTiming.*:LutInputArrival.*:LabPinmapTest.*:PlacedReduction*.*:TwelveReduction*.*:NarrowSearchTest.*'
ctest --test-dir build \
  -R '^nextpnr-mistral-(arrival-pin-assignment-cli|physical-lut-timing(-isolated)?)$' \
  --output-on-failure
```

Test logs are `build/issue206/final-focused-tests.log`,
`build/issue206/ctest.log`, and `build/Testing/Temporary/LastTest.log`.
`git diff --check` also passed. This is focused validation; the full repository
suite was not run.
