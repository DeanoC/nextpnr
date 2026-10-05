# GPIO input, output and output-enable registers

`FAST_INPUT_REGISTER`, `FAST_OUTPUT_REGISTER` and
`FAST_OUTPUT_ENABLE_REGISTER` used to pack only an input-only or output-only
pad with a register whose clock enable was tied high and whose clears were
inactive. Bidirectional pads, the OE register and register controls were not
supported, so an SDRAM data bus kept its registers in the fabric (the FES RAM
tester and Atari ST capture DQ in LABs for this reason).

The packer now absorbs, as Quartus Prime Lite 17.0.2 does:

| Pad | Registers |
| --- | --- |
| `MISTRAL_IB` | input (`MISTRAL_SDRIN`) |
| `MISTRAL_OB` | output (`MISTRAL_SDROUT`) |
| `MISTRAL_IO` (bidirectional or tri-state) | any of input, output and OE (`MISTRAL_SDRIO`) |
| `MISTRAL_IO` + `altddio_in` on its input | DDR input capture, optionally with output and OE registers (`MISTRAL_SDRIO`) |

with these `MISTRAL_FF` controls:

- clock enable, plain or inverted: `CEIN` for the input register and `CEOUT`,
  shared by the output and OE registers;
- asynchronous clear, plain or inverted, on the pad's shared active-high
  `ACLR`; each register enables it separately.

Polarity uses the programmable inverter in front of each pad control input.
Registers on one pad must share their enable and clear nets, and the output
and OE registers must share a clock. The input register may use a different
clock. One flip-flop cannot pack as both the input register and an output or
output-enable register. A register whose Q drives several pads
requesting the same register is copied into each pad, so one OE register can
drive a whole SDRAM bus. Synchronous clear or load, a clock enable tied low, a
clear held active, register parameters, inverted clocks, a Q that also drives
logic, and a pad input feeding more than the register are rejected.
`FAST_OUTPUT_ENABLE_REGISTER` on a pad without an output enable is ignored
with a warning. Quartus also has no I/O register sync clear: it folds the
clear into logic and leaves the register in the fabric, which nextpnr does not
do.

Decoded settings for the DQS16 lane of the pad:

| Register or control | Settings |
| --- | --- |
| output or OE register | `OEREG_HR_CLK_EN`, `RBOE_LVL_FR_CLK_EN` |
| output register | `OUTREG_OUTPUT_SEL` = `SEL_SDR` (data on `DATAOUT.0`, clock `CLKOUT.0`) |
| OE register | `OEREG_OUTPUT_SEL` = `SEL_1X`, `OEREG_POWER_UP_STATE` = 1, `OEIN.0` not inverted |
| input register | `RB_FIFO_WCLK_EN`, `RB_FIFO_WCLK_INV` (Q on `DATAIN.3`, clock `CLKIN.0`) |
| clock enables | `INPUT_PATH_CE_IN`, `CE_OUTREG_TIEOFF_EN`, `CE_OEREG_TIEOFF_EN` |
| asynchronous clears | `USE_CLR_INREG_EN`, `USE_CLR_OUTREG_EN`, `OEREG_ACLR_EN` |

The registers have no characterized setup/hold, clock-to-pad or clock-to-Q
model, so their pins are unclocked timing endpoints and nextpnr warns; a
fabric Fmax does not establish interface timing closure.

## Fixture

- `pads.v`/`pads.qsf`: eight pads, one per combination (output register with a
  combinational OE, OE register alone, input register alone on a bidirectional
  pad, output-only and input-only registers with clock enable and with
  asynchronous clear, and a tri-state output with output and OE registers).
- `bus.v`/`bus.qsf`: a four-bit SDRAM-style bus with one OE register, an
  active-low asynchronous reset and an active-low clock enable.
- `ddr.v`/`ddr.qsf`: two bidirectional pads built like the RAM tester's SDRAM
  DQ (`altiobuf_bidir` with `altddio_in` on the pad input), with packed output
  and OE registers.
- `split.v`/`split.qsf`: one bidirectional pad whose input and output
  registers use different clocks. Synthesis passes `-noclkbuf`, so packing
  inserts both global buffers. A netlist edit ties them to one clock, and
  another edit of `pads` makes one flip-flop serve both registers.
- `oracle/`: the Quartus projects (built with `QUARTUS` defined, inferring the
  tri-state pads), their gzip'd RBFs and `mapping.json`, the decoded per-pad
  settings and control-input inverters. The open flow instantiates
  `MISTRAL_IO` for the tri-state pads. The settings nextpnr writes by default
  and Quartus chooses per design are listed there and not compared.

`check.py` places and routes both designs, requires every pad's decoded
settings and control inverters to equal Quartus's, checks the clock-enable and
clear routes exist and that the bus registers are all absorbed, and checks
five unsupported requests fail.

```sh
python3 mistral/tests/io-registers/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/io-registers
```

This is host-only evidence. No board was programmed.
