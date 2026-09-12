# M10K simple dual-port independent clocks

`MISTRAL_M10K` accepts `CFG_DUAL_CLOCK=1` with write clock `CLK1` and
read clock `CLK2`. Both clocks are rising-edge clocks. The default remains
`CFG_DUAL_CLOCK=0`, using CLK1 for both ports and preserving the existing
single-clock bitstream configuration. A connected CLK2 requires the new flag;
missing required clocks produce a packing diagnostic.

The existing M10K BEL already exposes both clock inputs. Packing maps CLK1 to
CLKIN.0 and CLK2 to CLKIN.1. Independent-clock reads use ENABLE.0 to hold the
read address/core when B1EN is low. Bit generation selects the bottom clock,
read input/core clock enables and read domain selectors. The 40-bit input
half retains the write clock. CLK2 does not inherit the legacy unused bottom
clock inversion. No Mistral tables change. The retained Quartus configuration
oracles are in [oracle](oracle/README.md).

Timing now recognizes indexed A1DATA/B1DATA pins and supplies clocking info
for B1ADDR/B1DATA. Write inputs use CLK1; read address/enable and output use
CLK2 in the new mode. This preserves the existing M10K delay values; it does
not characterize new silicon delays.

Use Yosys with the paired `intel_alm` M10K inference changes. Old Yosys maps
independent-clock RAM to FFs; new inference emits CLK2 and CFG_DUAL_CLOCK=1,
including for memories whose two clocks are the same net. That new JSON
requires this backend. Old JSON remains supported.

Run the portable host regression (QSF/SDC are the DE10-Nano board files):

```sh
python3 mistral/tests/m10k/dual_clock.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/pins.qsf --sdc /path/to/clocks.sdc \
  --output /tmp/m10k-dual-clock
```

Run `legacy.py` with the same arguments to check new common-clock cells and
old JSON without CLK2/CFG_DUAL_CLOCK in both widths. Exact hardware artifacts
and recorded results are in [acceptance](acceptance/README.md).

This generates initialized 512x20 and 256x40 RAMs, each using one M10K, one
PLL, one HPS GP interface and no DSPs. It checks compressed RBF generation,
clock routing/selectors, a 50 MHz write-clock timing constraint and a 25 MHz
read-clock output timing arc, plus missing/inconsistent clock diagnostics.

For hardware, load the matching `20/top.rbf` or `40/top.rbf` under the kit
session protocol, then run `probe.sh 20` or `probe.sh 40` on the target. The
fixture derives both clocks from the board's 50 MHz reference. GPO bit 29
stops only the 25 MHz read clock, bit 30 controls read enable and bit 31
controls write enable. Writes require the explicit arming command in the
probe because the loader's post-program SPI probe also drives GPO. The test
reads every data bit through 16-bit windows, writes while the read clock is
stopped, resumes reads, and checks both enables. The 40-bit upper half is the
complement of the low 20 bits; test writes exercise high bits in both halves.

This is simple dual-port RAM, not two read/write ports or an asynchronous
FIFO implementation. Physical simultaneous read/write collision semantics,
CDC safety in user logic and unrelated clock-domain timing closure are not
established by these tests. The hardware fixture samples data only after the
host has allowed it to settle. Other M10K geometries retain their existing
mapping but are not covered by this hardware test.

For independent port widths, see [mixed-width SDP](README-mixed-width.md).

For two independently enabled read/write ports, see
[true dual-port M10K](README-true-dual-port.md).

For explicit read-during-write contracts and collision validation, see
[M10K read-during-write contracts](README-read-during-write.md).

For a combinational read port, see [asynchronous-read M10K](README-async-read.md).

For an explicitly registered read output, see
[registered M10K outputs](README-output-register.md).

For asynchronous clear controls, see [M10K ACLR](README-aclr.md).

For explicit address-stall controls, see [M10K address-stall](README-address-stall.md).

For a disabled true-dual-port side with a tied-off clock, see
[constant disabled-port clock](README-constant-clock.md).

For a design-scale audit of every packed M10K clock selector, see
[M10K design-scale selector audit](README-scale-selectors.md).
