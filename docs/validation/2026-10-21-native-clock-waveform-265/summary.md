# Native inverted-DDIO waveform diagnostic

This change binds the sampled AD20 local pulse-loss reference to the native
forwarder and analyser clock-route interval. It is an explicit diagnostic,
not a replacement for independent pad setup/hold arcs or a physical acceptance
model. Base: `4bd7d3e3f8114f95e43c164e149f4ecb70774b48`, branch
`feat/bounded-placement-swaps`, nextpnr draft #266 / issue #265, FES #840.

## Implementation

`getInvertedClockPulseLoss` requires the existing RAM Tester pad profile,
physical AD20 BEL, constant inverted DDROUT, supported electrical/default
selector settings, a valid existing pad-load bound, one supported parameter,
and no connected extra data/control ports. The existing load requirement also
keeps the primitive clock on the analyser's interval route calculation.

`timing/forwarded_clocks` accepts an AD20 declaration with exactly `port`,
`clock`, `load_pf` and `uncertainty_ps`. Only sampled 0/15/30 pF scenarios are
supported. The source must be the declared direct uninverted CLKBUF, with a
complete positive fixed-period waveform. Invalid/missing declarations,
configuration mismatches and negative/invalid route intervals fail closed.
The existing two-field AG5 OB contract retains its behavior.

The new record retains the source/input-driver and pad configuration, source
waveform, route interval, local losses and conditional output pulse estimates.
For route spread `late-early`, high uses source **low**, and low uses source
**high**; each subtracts spread, its local loss and explicit uncertainty. No
Quartus clock-pessimism-removal credit is imported. Negative outputs are not
clamped. A 4/6 ns asymmetric fixture verifies inversion, widening the route
interval worsens both pulses, and a uniform route latency shift cancels.

Actual emitted selector equivalence, source/PLL/buffer waveform distortion,
native fast-corner routing, board load/flight/slew and receiver requirements
remain physical obligations. A supported cell configuration is not an audit
of an emitted hardware artifact. Every record has `physically_qualified=false`;
`complete` stays false. No retention gate, inventory schema, bitstream emitter,
clock constraint, core RTL or interface protocol is changed. This is a
host-built native test binary; no production toolchain lock was changed.

## Validation

- Both focused native waveform tests pass (new AD20 and existing AG5).
- All **25** native-to-retention tests pass. The suite cross-checks the native 30 pF losses against
  the frozen Quartus reference and rejects even forged physical qualification
  on otherwise admissible inventories. Its log is retained.
- A fresh replay of the retained PC/XT placed graph passes. All **17,246**
  endpoint records, unsupported inventory, clocks, primitive/registered/
  unregistered contracts and existing HDMI/reset/static records match the
  prior capture exactly. Only `forwarded_clocks` and `physical_obligations`
  change. `final_analogue=false` and `complete=false` remain unchanged.
- In that **unrouted** graph, the interval is the prediction `[6850,6850] ps`.
  With the diagnostic 30 pF scenario and zero additional uncertainty, the
  conditional output high/low values are **4340/5250 ps**. These are not routed
  or board bounds. The zero uncertainty is a scenario input, not a measured
  source-clock property.
- Portable checking recomputes native/host arithmetic using the frozen Python
  reference, compares every old host field, verifies file hashes, and exercises
  forged-qualification rejection with the actual retention guard.

The first driver-test invocation omitted its required test-binary argument and
stopped before running tests; the corrected invocation is retained below.
A final incremental rebuild incorporated the tightened load guard before any
reported native tests. Build warnings concern existing narrowing/legacy code;
there was no compiler error. No new Quartus fit, PC/XT route, GPU use or hardware
operation occurred. The route allowance remains **1627.843715112 seconds**.

## Reproduce

Build the native test target from this source tree, then:

```sh
NEXTPNR_TEST_DDIO_WAVEFORM=/tmp/new-waveform.json /path/to/nextpnr-mistral-test \
  --gtest_filter='IoDelayTest.InvertedDdioWaveformKeepsRouteSpreadPolarityAndQualification:IoDelayTest.ForwardedClockContractKeepsPhysicalRouteAndBothEdges'
python3 mistral/tests/routed_swap.py /path/to/nextpnr-mistral-test
python3 docs/validation/2026-10-21-native-clock-waveform-265/check.py
```

`reproduce-host.sh` records exact retained local inputs for a new host replay;
set its binary/output-root variables and give a fresh result filename. The
portable checker needs only committed evidence and Python, not those caches.
Manifest hashes identify tested source bytes, test binary, input files and
retained captures. The binary was built before the result commit; its source
hashes, not a stale embedded version string, identify the tested implementation.

## Next integration boundary

A routed PC/XT capture must replace the predicted interval before interpreting
the new record as any routed evidence. Even then, the source-clock distortion,
fast-corner bound, emitted selector check and actual receiver/board budgets must
be resolved before physical acceptance. Existing independent SDRAM data/OE,
HPS, HDMI and reset obligations still apply. The compiler diagnostic does not
close FES #840 or qualify #836, and it does not justify a clock reduction or a
new production artifact.
