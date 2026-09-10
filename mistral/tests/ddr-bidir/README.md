# DDR bidirectional I/O

This fixture checks a one-bit Cyclone V `altddio_bidir` with changing fabric
data and output-enable nets. nextpnr packs the primitive and its constrained
`MISTRAL_IO` pad into one `MISTRAL_DDRBIDIR` cell. The pad's DQS16 site uses
`DATAOUT.1`/`DATAOUT.0` for the high/low output data, `DATAIN.3`/`DATAIN.2`
for the registered input samples, `DATAIN.0` for `combout`, and both `CLKOUT.0`
and `CLKIN.0` for the 50 MHz clock. `OEIN.0` carries the dynamic output
enable.

The supported profile is width one, `power_up_high=OFF`,
`oe_reg=UNREGISTERED`, `extend_oe_disable=OFF`,
`implement_input_in_lcell=UNUSED`, and `invert_output=OFF`. The two DDR clocks
must be the same non-inverted fabric clock. `inclocken` and `outclocken` are
constant high; `aset`, `aclr`, `sset`, and `sclr` are constant low; and
`oe_out`/`dqsundelayedout` are unused. Both data directions and OE must remain
connected to nets (a constant OE is also accepted). Unsupported parameters,
controls, clocks, widths, and malformed connections fail closed.

The implementation keeps the existing Mistral GPIO/DQS geometry and adds the
`MISTRAL_DDRBIDIR` packed cell type. `pack_ddr_bidir()` finds the
`MISTRAL_IO` output side used by Yosys for `padio`, releases the primitive's
output-net drivers, and rewires the GPIO BEL to `D_H`, `D_L`, `CLK`, `CLKIN`,
`OE`, `O`, `Q_H`, and `Q_L`. Bitstream generation selects the existing DDR
output and input FIFO settings for the site. No Mistral source-table changes
are required.

The Mistral timing database has no characterized bidirectional GPIO
setup/hold, clock-to-pad, or clock-to-fabric arcs. The fixture therefore
checks acceptance of the intended 50 MHz clock constraint and records the
empty interior Fmax section; this is not interface timing closure. It also
checks a compressed RBF, decoded DQS settings, and all GPIO data/clock/enable
routes. The test is host-only and does not program a board.

## Reproduce

```sh
python3 mistral/tests/ddr-bidir/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/ddr-bidir
```
`oe_out`/`dqsundelayedout` are unused. Both data directions and OE must remain
connected to a net (a constant OE is also accepted). Unsupported parameters,
