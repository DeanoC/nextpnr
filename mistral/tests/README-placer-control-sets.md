# Mistral HeAP joint placement and control affinity

Issue [#99](https://github.com/DeanoC/nextpnr/issues/99) reported a 600-second
C64 seed-5 placement timeout with timing weight 2000 and criticality exponent 5.
The original `s5-w2000-c5/route.log` progresses through six analytical
iterations: FF passes take 37–55 seconds, combined passes 49–69 seconds, and
M10K passes about 0.16 seconds. An isolated replay measured 0.16 seconds for
solving, 0.03 seconds for spreading, and 78.71 seconds for the first FF
legalisation pass. This is slow logic legalisation, not a stalled M10K loop.

Mistral now solves the movable LUTs and FFs together, using HeAP's existing
`placeAllAtOnce` mode. They share ALM input routing and LAB controls; separate
passes repeatedly move one type against the other's fixed placement.
Frozen shell cells remain locked during cart placement.

Ordinary placement also prefers nearby FFs with identical clock, enable,
asynchronous clear, synchronous clear and synchronous load signals, including
polarity. C64 has roughly 1,455 distinct FF control combinations, so grouping
only by clock does little to address enable incompatibility. HeAP now supports
control affinity: a LAB can contain several control signatures, and the full
backend legality check decides which may coexist. This preserves legal
sharing of different enables/resets and missing synchronous controls.

Other architectures retain HeAP's default exclusive control-set model. FES
carts also retain their exclusive clock/SCLR/SLOAD key. The bookkeeping now
removes an evicted cell's own signature, and ignores cells explicitly excluded
from the model (such as frozen shell FFs).

The existing `--placer-heap-no-ctrl-set` option disables affinity for comparison.
No timeout, M10K legality rule, timing target, or toolchain pin is changed.

## Regression

The shared bookkeeping has four native unit tests for duplicate residents,
mixed-set eviction, replacement and invalid removal. In an already configured
build, run:

```sh
git submodule update --init 3rdparty/googletest tests
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --target nextpnr-heap-control-set-test
ctest --test-dir build -R '^nextpnr-heap-control-set-test$' --output-on-failure
```

The target does not load a device database. Mistral CI builds and runs it.
All four tests passed locally. A broader native run also passed all 175
tests (171 Mistral tests plus these four; 534.76 seconds). The four shared
tests now build in the standalone target above. All seven CLI suites also
passed: timing-report-paths, LUT driver copy, decomposition, combinational
remap plans, remap plans, local remapping and reduction balancing.

Run from the source directory:

```sh
python3 mistral/tests/placer_control_sets.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --output /tmp/mistral-placer-control-sets
```

The fixture places 2,048 FFs with positive and negative edges of one clock,
mixed synchronous reset, asynchronous reset and enable choices, using the
issue's seed/weight/exponent. It checks joint analytical passes, determinism,
full placement legality, actual mixed-control LABs, and a placement difference
when affinity is disabled. Add `--width 2048` to exercise 8,192 FFs, roughly
C64's FF count. Logs, placed JSON and `summary.json` are saved. Runtime is
reported rather than asserted because it depends on the host. The test does
not route, use a GPU, build a bitstream or access hardware.

Local Release validation on 2026-10-01, with Mistral `7ed06e21`:

| Fixture | Affinity | HeAP time | Strict legalisation |
| --- | --- | ---: | ---: |
| 2,048 FFs | Enabled | 1.98 s | 0.58 s |
| 2,048 FFs | Enabled repeat | 2.04 s | 0.60 s |
| 2,048 FFs | Disabled | 2.12 s | 1.05 s |
| 8,192 FFs | Enabled | 12.62 s | 5.17 s |
| 8,192 FFs | Enabled repeat | 12.19 s | 5.11 s |
| 8,192 FFs | Disabled | 11.91 s | 5.72 s |

Repeated enabled placements were identical. Affinity improves the fixture's
legalisation time but does not universally improve total runtime or timing.
The existing `placer_options.py` five-variant regression and the complete
`fes_slot_region.py` suite (single region, dual region, region groups, frozen
cells and capacity failures) passed with the final configuration.

The architecture self-check (`--device 5CEBA2F17A7 --test`) fails before
placement at `bel != BelId()` in `common/kernel/archcheck.cc:91`. The same
failure reproduces with the existing toolchain at revision `655f3833`.

## Historical C64 replay

Current main rejects the original C64 synthesis JSON before placement at
`machine.firmware.lane9`: asynchronous M10K reads are unsupported. To measure
the reported placer issue without weakening that safeguard, the placement
changes were backported into an isolated source archive of the issue's pinned
nextpnr revision `0259c6dc1c46dd46fe79f3923a17ad36d2513421`, and built against
Mistral `7ed06e21`. Only the placement changes were applied; the original
synthesis JSON and QSF were untouched. The replay used:

```sh
nextpnr-mistral --json synth.json --device 5CSEBA6U23I7 --qsf socket.qsf \
  --seed 5 --placer-heap-timingweight 2000 --placer-heap-critexp 5 \
  --no-route --verbose --write placed.json
```

That historical revision forces the effective exponent to 7 despite the CLI's
5; the replay preserves that original behavior. Current main correctly honors
the CLI, as checked by `placer_options.py`.

Affinity alone and joint placement alone each still exceeded 600 seconds
(in refinement). Together, the full placement command completed successfully in **408.31
seconds**, wrote placed JSON and printed timing. Analytical HeAP took 359.99
seconds, including 351.60 seconds of strict legalisation. Placement timing
estimates were 44.53/52.22 MHz and 37.80/74.25 MHz (fail), and 138.77/12.29 MHz
(pass). The replay enabled joint placement only for ordinary designs; the
final change also enables it for carts, covered by the separate slot suite.
This distinction does not affect the ordinary C64 input. Detailed evidence is
recorded in `docs/validation/c64-placer-timeout-2026-10-01.json`.

The historical replay is placement-only diagnostic evidence. It does not
establish support for asynchronous M10K reads, routing success, timing closure
at 52.224/74.25/12.288 MHz, or a valid C64 bitstream. A legal synchronous C64
memory implementation remains necessary for validation on current main.
