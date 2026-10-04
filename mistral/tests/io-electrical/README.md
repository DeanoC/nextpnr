# 3.3 V I/O electrical options

nextpnr used to write every pad as 3.3-V LVTTL, 16 mA, fast slew and weak
pull-up off, whatever the QSF said. It now honours the Quartus instance
assignments below, with the encodings taken from Quartus Prime Lite 17.0.2
compiles on `5CSEBA6U23I7` and decoded with `mistral-cv`.

| Assignment | Values | Setting |
| --- | --- | --- |
| `IO_STANDARD` | `"3.3-V LVTTL"`, `"3.3-V LVCMOS"` | same input and 2 mA/16 mA output encoding |
| `CURRENT_STRENGTH_NEW` | LVTTL `4MA`, `8MA`, `16MA`; LVCMOS `2MA`; `MINIMUM CURRENT`, `MAXIMUM CURRENT` | GPIO `DRIVE_STRENGTH` |
| `SLEW_RATE` | `0` slow, `1` fast | GPIO `SLEW_RATE_SLOW` (both bits) |
| `WEAK_PULL_UP_RESISTOR` | `ON`/`OFF` | GPIO `USE_WEAK_PULLUP` |
| `ENABLE_BUS_HOLD_CIRCUITRY` | `ON`/`OFF` | GPIO `USE_BUS_HOLD` |
| `CLAMPING_DIODE` (obsolete `PCI_IO`) | `ON`/`OFF` | GPIO `USE_PCI_DIODE_CLAMP` |
| `D3_DELAY` | 0–7, combinational input | DQS16 `SET_T3_FOR_CDATA0IN/1IN` |
| `D1_DELAY` | 0–31, `FAST_INPUT_REGISTER` input | DQS16 `RB_T1_SEL_IREG_CFF_DELAY` |
| `D5_DELAY` | 0–31, combinational or `FAST_OUTPUT_REGISTER` output | DQS16 `RB_T9_SEL_OREG_DFF_DELAY` |
| `D5_OE_DELAY` | 0–31, combinational output | DQS16 `RB_T9_SEL_EREG_CFF_DELAY` |

Slew and drive strength do not apply to an input-only pad and are ignored, as
in Quartus. `D1_DELAY` without an input register is ignored with a warning,
matching Quartus warning 176437. Everything else fails closed: other I/O
standards, drive strengths an I/O standard does not offer (Quartus error
169055), bus hold together with a weak pull-up (169049), any series or
parallel termination (169058, no OCT on 3.3 V standards), out-of-range
delays, delays on paths whose encoding has not been checked (D3 on a
registered input, D5 on a DDR output, D5_OE on a registered output) and the
other D-chain assignments. Open-drain conversion is not implemented.

Assignment targets follow Quartus matching. `*` and `?` match any characters,
including bus brackets (`SDRAM_*`, `HDMI_TX_D[*]`, `d?2?`), and a bus name
(`-to q`) applies to every bit. A more specific assignment wins: wildcard
targets in file order, then the whole bus, then the exact pin name, as Quartus
does (warning 169156 in the oracle). Locations must name one pin.

The Mistral setter wrote only the low bit of a multi-bit boolean, so slow slew
needs Mistral with `MISTRAL_MULTIBIT_BOOL_SET`; without it `SLEW_RATE 0` stops
with an error and every other option still works.

## Fixture

- `top.v` and `pins.qsf`: one design using every option above on 24 pads. The
  Quartus build defines `QUARTUS` and infers its tri-state pads; the open flow
  instantiates `MISTRAL_IO`.
- `bus.v` and `bus.qsf`: wildcard, whole-bus and exact assignments that
  override each other.
- `oracle/`: the Quartus projects, their gzip'd RBFs and `mapping.json`, the
  decoded per-pad GPIO and DQS16 settings. The two settings nextpnr writes by
  default and Quartus chooses differently are listed there and not compared:
  `INPUT_REG4_SEL` on output lanes and the OE delay on a packed SDR output.

`check.py` synthesizes both designs, places and routes them with the same
QSFs, decodes the RBFs and requires every pad's settings to equal the Quartus
decode. It then checks 14 invalid requests fail before placement. With
`--reference-nextpnr` it also requires byte-identical RBF bytes from a binary
without this change when no electrical assignment is present.

```sh
python3 mistral/tests/io-electrical/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/io-electrical \
  [--reference-nextpnr /path/to/nextpnr-mistral-before]
```

This is host-only evidence: the bitstream settings equal Quartus's for the
same assignments. No board was programmed and no pin was measured.
