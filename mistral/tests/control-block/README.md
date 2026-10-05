# Control block atoms and device options

This fixture covers the Cyclone V control (CTRL) block atoms and the
device-wide QSF global assignments implemented in `mistral/ctrl.cc`. Every
mapping comes from Quartus Prime Lite 17.0.2 compiles of 5CSEBA6U23I7 decoded
with `mistral-cv`; `oracle/mapping.json` records the decoded facts, including
the experiments whose RBFs are not retained.

## Atoms

| Atom | Ports (atom -> CTRL block port) |
| --- | --- |
| `cyclonev_chipidblock` | clk -> CLOCK_CHIPID, shiftnld -> SHIFTNLD_CHIPID, regout <- REG_OUT_CHIPID |
| `cyclonev_crcblock` | clk -> CLOCK_CRC, shiftnld -> SHIFTNLD_CRC, crcerror / regout / endofedfullchip <- CRCERROR / REG_OUT_CRC / END_OF_ED_FULLCHIP |
| `cyclonev_opregblock` | clk -> CLOCK_OPREG, shiftnld -> SHIFTNLD_OPREG, regout <- REG_OUT_OPREG |
| `cyclonev_jtag` | tdouser -> TDOUTAP; tckutap, tdiutap, tmsutap, shiftuser, updateuser, clkdruser, runidleuser, usr1user; core inputs tckcore, tmscore, tdicore, corectl (CORECTL_JTAG), ntdopinena, output tdocore |
| `cyclonev_oscillator` (existing BEL) | oscena -> OSC_ENA, clkout <- CLK_OUT, clkout1 <- CLK_OUT1 |

Quartus sets no block, inverter or option bit for these atoms; they are pure
routing, apart from the CRC divider: `oscillator_divider` (1..256) becomes
`CRC_DIVIDE_ORDER` = log2(divider). The atom's divider wins over
`ERROR_CHECK_FREQUENCY_DIVISOR` (Quartus warning 176287, reproduced as a
nextpnr warning). The clock inputs accept a global clock (TCLK) or fabric
(TDMUX), both seen in Quartus. A Quartus round trip
(`decomp` -> `comp` -> `diff`) shows no undecoded bit for any of these designs.

`cyclonev_jtag` is the device TAP's USER0/USER1 data register as used by the
SLD hub: Quartus needs tck/tms/tdi/tdo connected to top-level ports (it rejects
an unconnected tdo, error 176551) and refuses the atom's own `tdoutap` port
(error 176304). Those four ports are the dedicated JTAG pins; nextpnr accepts
them connected directly to top-level ports (the Yosys blackbox marks them
`iopad_external_pin`) or unconnected, and trims the ports. This is direct atom
instantiation only: nextpnr has no `sld_virtual_jtag`/SLD hub insertion.

Rejected, with a message: `cyclonev_rublock` and `cyclonev_asmiblock` (Quartus
requires an Active Serial configuration scheme, which sets option bits Mistral
does not decode; MiSTer configures the FPGA from the HPS, so neither block is
usable there), a second instance of an atom, unknown ports or parameters,
non-default CRC error-correction parameters, a JTAG pin port driven by logic,
and `crcerror` driving a pad directly (Quartus forces the dedicated CRC_ERROR
pin for that).

Interface timing of the control block is not characterized in the Mistral
database; nextpnr warns and analyses only the fabric registers.

## QSF global assignments

Recognised names are checked; a value without a Quartus oracle fails closed.
Other global assignments remain ignored. Mapping (oram bit = Mistral option):

| Assignment | Effect |
| --- | --- |
| `STRATIX_JTAG_USER_CODE x` + `USE_CHECKSUM_AS_USERCODE OFF` | JTAG_ID = x. Without OFF Quartus ignores the code and programs its checksum, which nextpnr does not compute: error. `USE_CHECKSUM_AS_USERCODE ON` is rejected; the existing nextpnr output (FFFFFFFF) equals `OFF`. |
| `ENABLE_DEVICE_WIDE_RESET ON` | 6.4 = 0; DEV_CLRn on AB23 (1.8 V input) |
| `ENABLE_DEVICE_WIDE_OE ON` | 6.5 = 0; DEV_OE on AC24 (1.8 V input) |
| `ENABLE_INIT_DONE_OUTPUT ON` | 6.3 = 0 (Mistral's name RELEASE_CLEARS_BEFORE_TRISTATES_DIS is wrong); INIT_DONE open-drain output on AA20 |
| `ENABLE_NCEO_OUTPUT ON` | 2.19 = 0; nCEO push-pull output on AE25 |
| `ENABLE_CRC_ERROR_PIN ON` | 2.13 = 1, CRC_DIVIDE_ORDER from `ERROR_CHECK_FREQUENCY_DIVISOR`; CRC_ERROR open-drain output on Y19 |
| `CRC_ERROR_CHECKING`, `ERROR_CHECK_FREQUENCY_DIVISOR` | no bitstream effect alone (as in Quartus) |
| `RELEASE_CLEARS_BEFORE_TRI_STATES ON` | 6.2 = 0 (Mistral calls it CVP_CONF_DONE_EN) |
| `AUTO_RESTART_CONFIGURATION OFF` | 6.6 = 0 |
| `ENABLE_OCT_DONE ON` | 2.20 = 0 |
| `RESERVE_ALL_UNUSED_PINS_WEAK_PULLUP` | `AS INPUT TRI-STATED`: pull-up off on every unused GPIO pad; `... WITH BUS-HOLD`: also bus hold; default unchanged; the output-driving values are rejected |
| `CRC_ERROR_OPEN_DRAIN`, `INIT_DONE_OPEN_DRAIN`, `NCEO_OPEN_DRAIN` | only the default `ON` |

The START_UP_CLOCK decode overlaps bits 6.3-6.5, which are the three enables
above. The unused-pad set is the 315 pads Mistral reports as GPIO for the
U23 package: every bonded pad except the dedicated configuration and JTAG
pins, including pads of the HPS banks, exactly as Quartus does. A dedicated
pin that the design also uses is an error. Pad settings assume the 3.3-V
banks of the oracle (Quartus gives the same decode with or without
`STRATIX_DEVICE_IO_STANDARD`). The options are limited to 5CSEBA6U23 devices.

On the DE10-Nano all five dedicated pins carry MiSTer signals (SDRAM, VGA,
audio), so MiSTer designs cannot enable them.

## Regression

`check.py` compiles `top.v` (every supported atom) and `blinky.v`, decodes
the nextpnr RBFs and compares them with the retained Quartus RBFs: the same
set of CTRL ports and global-clock entries, the same CRC divider, no CTRL
setting, and for `options.qsf` and `bushold.qsf` the identical set of setting
changes relative to the plain design (332 and 600 settings). It then checks
that default and unknown assignments leave the RBF byte-identical and runs 17
rejected requests.

```sh
python3 mistral/tests/control-block/check.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv --output /tmp/control-block
```

Yosys needs the control-block blackboxes from DeanoC/yosys
`feat/intel-alm-control-block-atoms`. Host-only evidence: no board was
programmed, and nothing here shows the chip ID value, CRC error detection or
JTAG transfers working on hardware.
