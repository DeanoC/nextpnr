# Mistral router2 congestion regression

Run without Yosys, Quartus, or a board:

```sh
python3 mistral/tests/router2/congestion.py \
  --nextpnr /path/to/nextpnr-mistral --output /tmp/router2-m10k
```

The script decompresses the retained synth JSON, writes the one required
DE10-Nano clock-pin assignment and 50 MHz clock constraint, and runs seeds
1 and 2 using the default router (router2). Each run has a 120-second timeout;
use `--timeout` to adjust it, and repeat `--seed` to select seeds. It requires
successful routing, one M10K, compressed RBF output, and 50 MHz timing.

## Failure and fix

At nextpnr `131f880a856ee7f4b9b6379aa9b2c3e7fb001fe5`, seed 1 exceeds the
30-second diagnostic timeout with two wires overused by three excess nets.
The conflicting wires are H6.47.77.21 and V2.50.78.3. Three HPS GP signals
feeding the arm-detection LUT compete for them; the failure is not specific
to M10K data pins.

The source/sink coordinates give these nets an initial search box beginning
at column 47, including router2's three-column margin. Legal detours use
H6.46.77.21 and H6.46.77.31, outside that initial box. Router2 requests box
expansion every three congested iterations, but Mistral previously overrode
`expandBoundingBox` with an empty function. It could find overlapping routes
inside the box forever without considering the available detours.

Removing that override inherits BaseArch's one-tile expansion, clamped to
the device dimensions. Generic router2 code is unchanged. With the fix, the
retained seed-1 fixture reaches zero congestion in iteration 5; seeds 1 and 2
both finish in approximately four seconds on the development host. These
measurements describe this fixture and machine, not a general speed claim.

The former override cited slow TD congestion resolution. The existing M10K
byte-enable, independent-clock, same-clock and legacy fixtures are therefore
compared before/after in addition to the eleven-case mixed-width sweep.

## Multi-clock timing regression

The Mistral backend keeps router2 as its normal first pass. A design with at
least two PLLs and one M10K is retried with router1 when the routed timing
estimate has less than ten percent margin. This covers the denser multi-clock
case where the pre-bitstream estimate can be optimistic about the analogue
interconnect delay. The retry removes only ordinary routed nets; dedicated
global clock routes remain in place.

Run the retained ZX81 OSS fixture from the companion misteross checkout with
the three seeds used by the ladder:

```sh
python3 mistral/tests/router2/timing_qor.py \
  --nextpnr /path/to/nextpnr-mistral \
  --fixture /path/to/fes-zx81-oss/synth.json \
  --qsf /path/to/fes-zx81/constraints-oss.qsf \
  --sdc /path/to/fes-zx81/clocks-oss.sdc \
  --output /tmp/mistral-timing-qor \
  --timing-allow-fail
```

The script checks that both PLLs and an M10K are present, a compressed RBF is
written, and every constrained clock meets its signoff frequency. It accepts
`--seed` repeatedly for a smaller run. The fixture is intentionally supplied
by the caller because the generated JSON is several megabytes and belongs to
the core regression, not the nextpnr source tree.

## Fixture provenance

`m10k-mixed.json.gz` is the exact synth JSON from
`mistral/tests/m10k/mixed_width.v` in nextpnr
`071d60cafdab3adb23359824ca2c7f15c0aa8d44`, with UNIT=10, WLANES=4 and
RLANES=1, synthesized with the Yosys source committed as
`a18445dc033592cd568f2ac71317d94048042551`:

```text
synth_intel_alm -nolutram -nodsp -top top
```

The retained JSON preserves cell names and mapping, so this regression does
not depend on a future Yosys version generating the same netlist. Absolute
`src` attributes are provenance only; no local source paths are read when
routing it. The full RTL and original router1 hardware artifacts remain in
`mistral/tests/m10k`. Successful routing of the new RBFs is host evidence;
it does not inherit hardware acceptance from those older artifacts.

Mistral database revision: `78ba2a580ae2523403d4f4f91891a6b11d7b6aba`, unchanged.

SHA256 of decompressed synth JSON:

```text
7732dba2853d233486dc64bfddc41e977eaf1ac3917bab783f9eab825a0cd341
```

## Control comparison on the development host

Elapsed seconds include database import, placement, routing and bitstream
writing. Both versions used the same retained netlists and default router.
All cases produced one M10K and met 50 MHz.

| Case | Before iterations | After iterations | Before seconds | After seconds |
| --- | ---: | ---: | ---: | ---: |
| byte | 4 | 4 | 4.03 | 3.88 |
| dual/20 | 5 | 5 | 3.78 | 4.09 |
| dual/40 | 10 | 8 | 4.03 | 4.03 |
| legacy/20/same-clock | 12 | 5 | 4.08 | 3.88 |
| legacy/20/legacy | 4 | 4 | 3.93 | 4.18 |
| legacy/40/same-clock | 6 | 9 | 3.93 | 3.83 |
| legacy/40/legacy | 5 | 6 | 3.88 | 3.88 |
