# One PLL output with gated and ungated branches

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, V11 50 MHz reference,
25 MHz integer PLL output, direct mode, zero phase and 50% duty. The same
PLL output clocks an always-running eight-bit counter and feeds a
`cyclonev_clkena` that clocks a separate eight-bit counter. HPS GP output bit 0
controls the gate. GP input bits 7:0 observe the gated counter, bits 15:8 the
running counter, and bit 16 PLL lock. No hardware was programmed.

The gate requests global clock, falling-edge enable registration, power-up
low, disable mode low and test_syn high. `enaout` is unused. This is a
host-only configuration oracle, not hardware acceptance of stop/resume.

The PLL is fixed at `FRACTIONALPLL_X0_Y15_N0` (FPLL.000.014), and its single
output counter at `PLLOUTPUTCOUNTER_X0_Y20_N1` (C6, divide 12). Quartus uses
M12/N2 with a 300 MHz VCO. The gated branch is constrained to `CLKCTRL_G3`
(CMUXHG.000.035 lane 3), and the ungated wrapper clock enable to `CLKCTRL_G2`
(lane 2). The fitter confirms both global clocks share the same physical
C6 counter; there is no duplicate counter or PLL.

Both CMUXHG lanes have INPUT_SEL `16` hex and TESTSYN_ENOUT_SELECT
`PRE_SYNENB`. Lane 2 remains always enabled (ENABLE_REGISTER_MODE default
`VCC`). Lane 3 uses ENABLE_REGISTER_MODE `REG1_ENOUT` and
ENABLE_REGISTER_POWER_UP `0`, with its fabric enable entering
`CMUXHG.000.035.3:ENABLE` through `TD.000.036.0017`.

`top.v`, `oracle.tcl`, `pins.qsf` and `clocks.sdc` are build inputs. `top.qsf`
is the exported assignment file with its source path made relative.
`fitter-pll.txt` preserves PLL and global clock usage; `pll-settings.txt`
contains all emitted FPLL/CMUX settings, reset inversion and the enable
endpoint route. Omitted settings retain Mistral defaults. Load the preserved
RBF for complete settings comparisons. `top.rbf.gz` contains the exact
Quartus RBF compressed with gzip mtime zero, and `sha256.json` identifies
both raw and compressed bytes.

Regenerate in an empty output directory:

```sh
quartus_sh -t /path/to/fixtures/clock-branches/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Compilation passed with zero errors and four warnings, including the tied PLL
reset and Lite LogicLock notices. Placement/routing may vary between runs;
the hashes identify this preserved output. Mistral decoder commit:
`78ba2a580ae2523403d4f4f91891a6b11d7b6aba`.
