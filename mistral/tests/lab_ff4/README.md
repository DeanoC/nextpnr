# Secondary ALM registers and the second LAB clock

Each Cyclone V ALM has four registers, but nextpnr only placed flip-flops on
FF0 and FF2 (`is_alm_legal`: "TODO: why are these FFs broken?", 2021). The
LAB model also allowed one clock signal of one polarity per LAB. Two opt-in
options lift this:

- `--mistral-ff4` places flip-flops on FF1 and FF3 of plain LABs.
- `--mistral-clkb` uses a LAB's second clock source (CLKB) and the
  per-pair clock inverters, so two clocks, or `clk` and `~clk`, share a LAB.

Without either option the routing graph, legality and bitstream are
unchanged. The default RBFs of this fixture's neighbours (`lab_aclr`,
`lab_legalise`, `ff_datain`, `lut_mask`, `sdr-output`, `ddr-input`) and of
a 2,505-cell Spectrum design are byte-identical to nextpnr 3d4a5b35.

## Why FF1/FF3 were broken

Quartus 17.0.2 oracles (`oracle/`, LAB X30 Y20, registers locked to
`FF_X30_Y20_N<n>`) show:

| Quartus site | nextpnr | output | packed-input bit | clock+enable, async clear |
| --- | --- | --- | --- | --- |
| N(6a+1) | FF0 | FFT0 | TPKREG1 | TCLK_SEL, TCLR_SEL |
| N(6a+2) | FF1 | FFT1, FFT1L | TPKREG0 | BCLK_SEL, BCLR_SEL |
| N(6a+4) | FF2 | FFB0 | BPKREG1 | BCLK_SEL, BCLR_SEL |
| N(6a+5) | FF3 | FFB1, FFB1L | BPKREG0 | TCLK_SEL, TCLR_SEL |

The data path (LUT output or the half's E/F via EF_SEL), PKREG, SCLR_DIS
and SLOAD_EN follow the ALM half. The clock+enable and async-clear
selectors do not: FF1 is clocked and cleared with FF2, and FF3 with FF0.
nextpnr wired FF1 to TCLK_SEL and FF3 to BCLK_SEL, so a secondary register
took the other half's clock (or none at all when that half was empty). The
first FF bitgen also had the PKREG mirroring still wrong when the TODO was
written; that was fixed 45 minutes later in 8bc9732d, but FF1/FF3 were never
re-enabled. The PKREG, EF_SEL and output-mux encodings nextpnr already had
are confirmed by these oracles; the defect was the control grouping.

Single-register oracles `one_n1`..`one_n5` and WYSIWYG `cyclonev_ff`
oracles `w_sclr_n*`/`w_sload_n*` isolate each selector. Two-register
partitions `g_*` (two global clocks), `i_*` (clk/~clk), `r_*` (async
clears) and `e_*` (enables) are accepted by Quartus only as
{FF0, FF3} / {FF1, FF2}.

## Legality with `--mistral-ff4`

- FF0+FF3 must share clock, clock polarity, enable and async clear; so
  must FF1+FF2. A register without an async clear still follows its
  group's TCLR_SEL/BCLR_SEL, so it cannot share a group with a cleared one.
- Two registers in one half must share SCLR and SLOAD (per-half enables).
  A real sync load or any SDATA keeps its half to one register (Yosys never
  infers SLOAD; shared-SDATA cases are not proven).
- A half holding both registers owns FFx0, FFx1 and the local FFx1L
  outputs, so its LUT may feed only those two registers. Quartus rejects
  even a same-LAB consumer (`rejected/lutfan`, `rejected/lutfan_local`).
- Both registers of a LUT-less half need data: one through a route-through
  LUT, one packed through E/F. In arithmetic ALMs E/F availability is
  checked against the D0/D1 pins the adders reserve.
- MLABs keep the two-register model: Mistral's MLAB clock tables were not
  corrected or checked against Quartus.

Either option also switches plain LABs to an exact LAB control-set model
(`LabPairWorker`): three clock+enable pairs, a dedicated clock enters on
CLKIN and uses no DATAIN line, a pair without an enable uses none, ENk comes
from DATAIN 2/3/0, async clears from DATAIN 3/2. The default model charges
DATAIN[0] to every clock and a DATAIN line to every enable-less pair, so it
rejects e.g. two async clears in one LAB that Quartus uses (`r_abba`).

## `--mistral-clkb`

CLKA comes from CLKIN[0] or DATAIN[0] (CLKA_SEL=DIN0), CLKB from CLKIN[1]
or DATAIN[1] (CLKB_SEL=DIN1). Pair k selects CLKA or CLKB (CLKk_SEL=CLKB)
and may invert it (CLKk_INV). nextpnr adds CLKIN[1]/DATAIN[1] pips into the
three LAB clock wires and reserves the exact source of every used pair
before routing. At most two clock signals per LAB (`rejected/clk3_alm`).
Without `--mistral-clkb`, a LAB keeps one clock of one polarity.

## Check

`check.py` decodes every stored Quartus RBF, re-derives the register facts
(TCLK/BCLK/TCLR/BCLR selectors per register, SCLR_DIS, the used clock pairs'
CLKk_SEL/CLKk_INV/ENk_EN/ENk_NINV, CLKA_SEL/CLKB_SEL, ACLRj_SEL/INV) and
compares them with `oracle/mapping.json`. It then builds a structural
netlist with every register (and LUT) locked to the oracle's site, runs
nextpnr with the case's options and requires identical facts from its RBF.
Which register of a LUT-less half gets the route-through is a free choice
(Quartus picks either; compare `loc4` and `ena3_alm`), so PKREG is checked
against the routed E/F data path instead. Finally a routed design is reloaded
without each option (refused) and with both (same RBF).

```sh
python3 mistral/tests/lab_ff4/check.py --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral --mistral-cv /path/to/mistral-cv \
  --output /tmp/lab_ff4
```

`tests/lab_ff4.cc` (gtest, `BUILD_TESTS=ON`) covers the rejection rules,
since BEL-locked cells bypass `isBelLocationValid` in the CLI flow.

## Not covered

- No hardware run. A kit test is needed before either option becomes the
  default; these are host-only Quartus-encoding comparisons.
- MLAB secondary registers and MLAB CLKB.
- Register feedback modes (TMODE F_0/D_F): Quartus absorbs a clock enable
  into the LUT through the FF feedback path (`ena_2ff`); nextpnr does not
  model this.
- Global async clear: Quartus selects ACLR0_SEL=ACLR0 with the clear on the
  dedicated network into `LAB:ACLR.0` (oracle `aclrg_abba` in the work
  area); not implemented.
- Shared arithmetic (Quartus sets SHARE=1 with ARITH_SEL=ADDER for a
  three-input adder) and seven-input extended LUT mode: not implemented.
