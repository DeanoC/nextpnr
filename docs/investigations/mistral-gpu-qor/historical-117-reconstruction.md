# Reconstructing the historical profile

9 October 2026. The missing historical artifacts are treated as unavailable.
This experiment rebuilds the pass sequence using the frozen reconstructed old
input and newly qualified guide reports. It does not reproduce the authenticated
117.343346 MHz run. Production code, default settings, FES recipes, RTL and
hardware are unchanged.

## Completed controls

All rows below use reconstructed synthesis SHA256
`f90717c8f06708485a125aad1488531a45f9f92e725bad0a969c8a5ddafe0b65`,
BUILD_ID `7168b508ab424b70f1c0f87b2035d821`, old QSF, unchanged board SDC,
seed 2, device `5CSEBA6U23I7`, HeAP weight/exponent 10/2 and enable replication
budget 4. Output comes from completed RBF generation and final analogue timing.
This is reported timing; complete setup/hold signoff is not established.

| Compiler lane / reconstructed profile | Memory MHz | Pixel MHz | Capture MHz |
| --- | ---: | ---: | ---: |
| Current: unstacked | 87.229591 | 78.634895 | 285.474762 |
| Current: pre-placement reduction | 90.810028 | 79.339890 | 288.259857 |
| Current: reduction + three local remaps | 91.058090 | 80.160316 | 292.277893 |
| Current: reconstructed remaining-family subset | 90.867790 | 79.214195 | 292.277893 |
| Older retained compiler: unstacked | 105.775330 | 96.320557 | 375.087128 |
| Older retained compiler: reduction + three local remaps | 104.123283 | 87.573349 | 341.579620 |
| Published historical source: unstacked | 106.157112 | 94.500099 | 347.793488 |
| Published historical source: reduction + three local remaps | 108.318886 | 95.129372 | 305.033325 |
| Published historical source: reconstructed complete family stack | 114.324913 | 93.231400 | 378.582397 |

Pre-placement balancing gains 3.580437 MHz. Three fresh local stages add
0.248062 MHz: total 3.828499 MHz versus the current unstacked control.
The larger subset (adds internal cut, capture locality, the second placed
reduction, decomposition and driver copy; omits unavailable first placed
reduction) finishes at 90.867790 MHz. It gains 3.638199 MHz versus unstacked
but loses 0.190300 MHz and 23 ps of memory setup margin versus the three-local
profile. Pixel loses 0.946121 MHz versus that simpler profile; capture is
unchanged. It preserves their measured hold minima. More qualified passes
do not imply a better completed route.
This is a **full-flow mapping, placement and routing improvement** on one
reconstructed input/seed, not a router-only or current-RTL result.

The current memory setup WNS changes −3.772 → −3.320 → −3.290 ns against the
unchanged 130.005203 MHz constraint. All three reported clock Fmax values
improve. Final holds remain positive, but balancing reduces memory/pixel/capture
hold headroom by 6/35/270 ps. The local stages preserve those measured hold
minima. Reserve qualification and complete interface coverage remain separate;
this design still fails the memory setup requirement and has no hardware
acceptance.

The older control matches the previously retained same-input result exactly
in reported Fmax after moving from GPU1 to GPU0. Logged checksums differ, so
this is **not** proof of identical final routes. The older compiler does not
export the current native `timing_summary`; absent setup/hold aggregation is
unreported, not passed. Cross-compiler results change compiler, dependencies,
packing, placement, routing and GPU as well as the timing model. They do not
isolate the numerical effect of a timing-model correction.

The freshly rebuilt published historical source finishes its unstacked control
in 1,232.64 seconds at **106.15711212158203 MHz** memory Fmax. This matches the
recorded historical fixed-input control at its published six-decimal precision.
It is encouraging numerical evidence for the reconstruction, not proof of
identical synthesis, routing or RBF bytes. The missing original input hash
still differs from the reconstructed input, and the original executable is
unavailable. The fresh complete family stack reaches **114.32491302490234 MHz**, gaining
8.16780090332031 MHz versus this compiler-matched unstacked control. It remains
3.0184326171875 MHz below the recorded driver-copy result. This reconstructs
all recorded pass families with fresh choices; the original candidate identities
and authenticated 117 MHz run have not been reproduced. Its capture Fmax
matches the historical driver-copy record, but that does not authenticate
the rest of the design or route.

Its reported worst same-edge memory path launches from HPS `f2sdram`
`cmd_port_clk_1` and ends at `m1_burstcount[7]` FF ENA. It totals 9.420 ns against
a 7.692 ns window (−1.728 ns): 7.422 ns routing, 0.800 ns logic, 1.177 ns
clock-to-Q, +0.217 ns clock contribution and −0.196 ns setup. The native
clock-window aggregate is absent in this old source, so complete setup/hold
coverage is not claimed. Neither the 130 MHz target nor hardware acceptance
is achieved.

## Fresh guide dependencies

Both compilers independently qualified three local stages with groups 8/1/1,
FF placement preservation and pin optimization disabled. Fresh candidate 0
is specific to each reconstructed graph/report. The historical candidate
identities and authenticated guide reports remain missing.

Placement-only reports produced zero qualified internal-cut candidates in
both lanes. Retrying with the completed current three-local route correctly
failed native validation: routing-created cells/ports do not exist in the
pre-routing graph. The raw report and refusal are retained.

A separate selection guide keeps **9,748 whole unchanged paths** out of 15,290:
4,882 paths reference absent cells and 660 reference absent ports. Of the
retained paths, 1,698 are violated. Each retained endpoint, placement and
routing edge matches the fresh placed graph. No segment, delay, clock label
or setup window is edited. Native path validation and all candidate legality,
Boolean, affected-sink, clock-pair and hold checks remain required. This partial
guide supplies selection hints; it is not a signoff report or complete coverage
proof. It produces four qualified cuts; fresh candidate 0 is a port1
`reads_after` cut and applies successfully. It is not established as the original
historical cut.

Capture locality retains 24 moves (historical record: 60). The first placed
root is unavailable under native qualification; the second qualifies at the
recorded 200 ps minimum and applies. Control decomposition also applies, and
driver copying qualifies 64 alternatives and selected candidate 0 applies.
The current subset completes routing and final signoff. Memory setup WNS is
−3.313 ns, pixel +0.844 ns, capture +2.776 ns. Holds are +0.753/+0.744/+3.751 ns
(memory/pixel/capture). The older three-local route completes at 104.123283 MHz, a 1.652046 MHz
regression versus its control. Pixel and capture also regress. This strengthens
the need for source-matched reconstruction and fresh candidate identities.

A third lane has built pristine published source `ae5350d7`, whose record
states its compiler/test bytes match the measured source. This is a freshly
built compiler with selected retained dependency `7ed06e21`, not the missing
original executable or independently authenticated original dependency/build
environment. Its source and dependency archives, build flags and binary have
been recorded. Its bulk unit process reached about 37 GB RSS and was stopped
after 160 passing cases. The remaining ten runnable cases pass in a separate
process; one fixture-dependent preflight is skipped. The interrupted bulk
suite is not a pass. All seven native CLI checks pass, including the last
reduction-balance check resumed after interruption. The published compiler
qualifies a different first local cut: the DDR0 `group_valid`/error-enable cone,
with two ENA users, rather than the HPS slot-free cone selected by the other
compilers. It applies successfully. The next fresh stage qualifies an HPS
port0 slot-free cut at `MISTRAL_COMB.24.31.0` with two ENA users; its selected
placement completes, followed by the third stage and full route. The three-local
profile reaches 108.318886 MHz; capture falls to 305.033325 MHz. Completing the
remaining families adds 6.006027 MHz memory and recovers capture to
378.582397 MHz. Pixel ends at 93.231400 MHz, 1.268700 MHz below this compiler
control. Headline clock changes are distinct from required-window acceptance. This is an observed candidate difference; the historical guide
identity remains unproved. It takes priority over
completing another approximate retained compiler stack.
Unavailable candidate families will be listed explicitly; a resulting subset
will not be relabeled the exact historical stack. No predicted guide Fmax is
included in the completed-results table.

## Confirmed placement-policy differences

The published source `ae5350d7` leaves HeAP's `placeAllAtOnce` at its default
false. It configures FF control-set grouping only for region-constrained FES
slots. Ordinary RAM-test placement therefore has no such grouping.
The measured current compiler source `9cf5fc12` sets `placeAllAtOnce = true`,
solving movable LUTs and FFs together. It also enables ordinary FF control-set
search affinity, with nonexclusive groups keyed by clock, synchronous clear,
synchronous load, enable and asynchronous clear (including inversions).
Full architecture legality still decides which sets can share a LAB.

Commit `4e5ace3c493cd38d9f1fa2a8bd4337b131b04646` introduced both changes
to address HeAP placement timeouts. These are confirmed differences in
`mistral/arch.cc` and the HeAP configuration,
not measured causes of the timing gap. In particular, the historical ordinary
flow did not use exclusive control-set grouping. A future controlled ablation
must separate joint placement and control-set affinity from dependency/timing
model changes. Neither policy has been changed in this reconstruction.

## Provenance and next decision

Raw logs, exact commands, GPU identity, input/binary stability checks, hashes,
plans, guide receipts and outputs are retained under:

`build/investigation/recreate-117-profile-20261009/`

The current lane uses sealed clean source
`9cf5fc12f0f91ffc279a46adb0d9c1f2a27d768e`, binary SHA256
`0c76fd861248a5e272ec786306de4c8dd88b56d436141715b15dbb78d6304c60`,
GPU1 R9700. The older lane uses retained binary SHA256
`fd28a85f6edee106ca79d4fd483063a4eb40e3928c36e3749c8d813dadee1f27`,
base `914f8fed3ce8d3e4bbe18bee2a6d7bdacfead420` with recorded dirty-diff
SHA256 `55f72bf0581eb7ed3bf6aa349940c6ba719b1d86f1925bb15ff006259b1086d0`,
GPU0 RX7900XTX. Neither binary is the missing measured historical `5dd69a8d`.
Each paired lane keeps its own GPU fixed. Temporary output uses the main disk.

The compact [completed measurement record](../../validation/mistral-historical-117-reconstruction-2026-10-09.json)
contains final-window observations from the existing QoR helper and worst
same-edge memory-path component attribution. These are actual reported paths,
which can change between full-flow profiles; their component differences are
not controlled single-arc measurements. The independent full historical
physical/equivalence proof has not been recreated for this new stack.

The published final stack applies all families, including both placed roots,
with **43** retained capture moves versus the historical **60**. Its fresh comb
cut targets the DDR1 burst-enable cone; decomposition targets DDR1 nack/cmd-end;
the driver copy targets DDR0 burst/cmd-following arithmetic at
`MISTRAL_COMB.33.31.0`. Candidate ordinal equality alone does not prove historical
mapping or placement equality. A stale raw routed guide was refused; selection
uses 9,682 whole compatible paths (691 violated) without editing path delays or
clock windows. That partial guide remains selection evidence, not signoff.

The final worst same-edge memory path is HPS `f2sdram.cmd_port_clk_1` to port1
`skid_address[28]` FF ENA: 8.747 ns against 7.692 ns (−1.055 ns). It contains
6.757 ns routing, 0.800 ns logic, 1.177 ns clock-to-Q, +0.209 ns clock contribution
and −0.196 ns setup. All six other **reported** setup clock-pair minima are
positive, including falling-to-rising memory +2.070 ns and capture-to-memory
+3.283 ns. This old report exports no hold aggregate or hold paths; complete
setup/hold acceptance, interface coverage and hardware acceptance are therefore
not claimed.

Next: replay the identical qualified full profile on free GPU1, keeping source,
input, constraints, seed and all selection guides fixed. Full placement and
native guide validation rerun; actual placed geometry must match before treating
this as a GPU routing comparison. This isolates GPU-dependent behavior without
adding a production setting. Then separately ablate joint placement and ordinary
control affinity with the current timing model, rather than attributing the
cross-compiler difference to a model correction alone.
Useful opt-in choices then need current-RTL and paired-seed confirmation before
proposing a recipe/default change. The separate three-net rerouting experiment
remains pending.

## Cross-GPU replay diagnostic

The identical qualified profile on GPU1 failed after 548.38 seconds during
backend initialization: HIP returned `invalid argument` at a host-to-device
`hipMemcpy`. Input and compiler hashes remain unchanged. It produced no
completed route, timing comparison or RBF; the earlier GPU0 result remains
114.324913 MHz.

A small standalone host probe links the published build's actual static GPU
backend. On free GPU0 it passes initialization and a two-hop route both on a
three-wire graph and a synthetic graph with the full RAM-test resource counts
(2,740,007 wires, 27,175,224 edges). Those are backend diagnostics, not design
performance or physical acceptance. Once the unrelated owner finished, GPU1
passed the same small and resource-count probes, plus a resource-count test with
only GPU1 visible and another with no prior device-description call. The full
flow failure is not reproduced by these tests. An unchanged full-profile retry
is running in a fresh directory; no source fix or runtime filter was applied.

The historical/current backend header and HIP wrapper match. The kernel file's
source difference adds PCI/UUID identity reporting; search and transfer code is
unchanged. Both `gfx1100` and `gfx1201` are present in the historical build's
configuration. These checks do not establish the initialization failure's cause.

The free GPU0 is also running a paired driver-copy ablation: the same published
source, input, seed, constraints, selected local/comb/capture/reduction/decomposition
stages and GPU, with only driver copying disabled. This compares completed full
flows; it is not a fixed-placement routing-only experiment. The 114.324913 MHz
with-copy run is its reference. No result is available yet.

10 October update: the unchanged GPU1 retry also failed at the same HIP
host-to-device upload after 557.53 seconds. The GPU0 no-driver control failed
there after 572.08 seconds. Neither produced a new route, timing result or
option-benefit comparison. Both failures preserve input/compiler identities.
This now occurs in full flows on both devices despite the standalone backend
probes passing. The next diagnostic must identify the failing upload and its
buffer state in the full process before another expensive placement retry.
The best completed result remains 114.324913 MHz; driver-copy benefit in this
reconstruction is still unmeasured.


## Bounded upload trace (10 October)

A dynamic HIP trace calls the original allocation/copy API before logging metadata.
It deliberately exits 77 after nine successful graph uploads, before scratch
allocation, routing kernels or final timing. The retained real placed graph passes
all nine in 5.753 seconds. A fresh full-process GPU0 no-driver control also passes
all nine in 563.242 seconds, with unchanged compiler and input hashes. No stale
remap guards were bypassed, and the compiler source is unchanged.

The earlier full-flow upload failures are therefore not reproduced under this
trace. Logging/preloading can alter host memory layout or timing, so no fix or
root cause is established. Exit 77 is an intentional diagnostic stop, not a routed
result. Raw logs, exact commands and hashes are retained in
`build/investigation/recreate-117-profile-20261009/hip-upload-diagnostic`.
Next, run the traced control through routing to capture any later failure. Its
result must retain the instrumentation label; a completed run alone does not
establish a clean driver-copy ablation against the uninstrumented reference.


The subsequent complete instrumented GPU0 no-driver control finished successfully
in 815.284 seconds: memory 115.366867 MHz, pixel 94.625282 MHz, capture
300.242950 MHz. Input hashes remained unchanged. It includes the HIP trace
preload and has no matching instrumented with-driver reference, so it does not
isolate driver-copy benefit or establish a fix for the initialization failure.
The selected uninstrumented research baseline remains 114.324913 MHz.
