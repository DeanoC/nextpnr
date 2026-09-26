# Fabric-data DDR output registers

This fixture checks a one-bit Cyclone V `altddio_out` whose `datain_h` and
`datain_l` are changing fabric nets. nextpnr packs the primitive and its
directly connected output buffer into one `MISTRAL_DDROUT` cell. The two data
nets use the pad's dedicated `DATAOUT.1` and `DATAOUT.0` lanes, and the output
clock uses `CLKOUT.0`.

The supported profile is width one, `power_up_high=OFF`,
`oe_reg=UNREGISTERED`, `extend_oe_disable=OFF`, and `invert_output=OFF`.
`outclocken` and `oe` must be constant high; `aset`, `aclr`, `sset`, and
`sclr` must be constant low; and `oe_out` must be unused. Both data inputs
must be connected to nonconstant nets. Complementary constant data remains
supported by the separate [DDR clock-forwarding fixture](../ddr-output).
Wider primitives, missing or constant data, dynamic controls, invalid clocks,
unsupported parameters, and malformed output connections fail closed.

The implementation adds the `D_H` and `D_L` BEL pins in `mistral/io.cc`, maps
them to `GPIO DATAOUT.1` and `DATAOUT.0`, and extends `pack_ddr_outputs()` to
retain the two fabric nets when it rewrites the output buffer as
`MISTRAL_DDROUT`. `mistral/bitstream.cc` selects the Cyclone V DDR output
register and leaves both variable-data lanes non-inverted. The Quartus
17.0.2 oracle reports the same geometry at W15:
`DQS16.089.008` lane 9, `DATAOUT.0`, `DATAOUT.1`, and `CLKOUT.0`, with
`OUTREG_MODE_SEL=DDR`, `OUTREG_OUTPUT_SEL=SEL_SDR_DELAY`, and
`RBOE_LVL_FR_CLK_EN=1`.

The Mistral timing database has no characterized GPIO register setup/hold or
clock-to-pad arcs. `D_H` and `D_L` are therefore timing endpoints and `CLK` is
a clock input; nextpnr emits a warning that the reported fabric Fmax does not
establish output-interface timing closure. The fixture checks the intended
50 MHz fabric constraint, routing, utilization, compressed RBF generation,
and the decoded lane/settings oracle. It is host-only and does not program a
board or claim a measured waveform.

## Reproduce

```sh
python3 mistral/tests/ddr-output-data/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/ddr-output-data
```

The [oracle](oracle) contains the small Quartus Prime Lite 17.0.2 project,
the retained compressed RBF, and a settings/hash manifest. Decompress the
oracle RBF and run `mistral-cv decomp 5CSEBA6U23I7` to inspect it. The oracle
is a geometry and setting reference, not a bit-for-bit compatibility claim.

The [host artifact](host-artifacts/top.rbf.gz) is the compressed nextpnr RBF
from the passing fixture. Hashes, utilization, timing, and the host-only
classification are recorded in [host-results.txt](host-artifacts/host-results.txt).
