# Two independent PLLs from the board reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, one 50 MHz reference
on V11, independent 25 MHz integer and 12.288 MHz fractional outputs,
direct mode, zero phase and 50% duty. Each clocks its own eight-bit counter;
the HPS GP input observes both counters and lock bits. There are no
cross-clock register paths. This is a host-only reference, not hardware acceptance.

The two feedback configurations cannot share a PLL. PLL and output-counter locations are constrained to the following sites,
with output-global paths left to Quartus:

| Output | Quartus PLL site | Mistral site | Counter | Global path |
| --- | --- | --- | --- | --- |
| Video, 25 MHz | FRACTIONALPLL_X0_Y15_N0 | FPLL.000.014 | C6, divide 12 | VG42,0 lane 3, INPUT_SEL `11` hex |
| Audio, 12.288 MHz | FRACTIONALPLL_X0_Y32_N0 | FPLL.000.031 | C6, divide 33 | HG lane 0, INPUT_SEL `0e` hex |

Both PLLs report a dedicated-pin reference sourced by `FPGA_CLK1_50~input`.
Video uses CLKIN(0), `CLKIN_0_SRC 4`; audio uses CLKIN(2), `CLKIN_0_SRC 6`.
Thus the board reference reaches both sites directly. The video counter is
`PLLOUTPUTCOUNTER_X0_Y20_N1`; audio is `PLLOUTPUTCOUNTER_X0_Y33_N1`. Video uses M12/N2 and a 300 MHz VCO.
Audio uses M8/N1 plus fractional word `1c2e33f0`, with a 405.504 MHz nominal
VCO. Its odd counter uses high 17 / low 16 with even-duty correction.
The audio HG lane uses `PRE_SYNENB`. The only emitted auxiliary bandgap powerdown
is at FPLL.000.073; the two occupied sites remain enabled.

`top.v`, `oracle.tcl`, `pins.qsf` and `clocks.sdc` are the exact build inputs.
`top.qsf` is the exported assignment file with its source-file path made relative.
`fitter-pll.txt` preserves the PLL usage report. `pll-settings.txt` includes all
non-default FPLL and clock-mux settings and reset inversions. Absent settings
are defaults, not zeros: compare complete FPLL settings by loading the RBF.
`top.rbf.gz` is the exact Quartus output, gzip-compressed with mtime zero;
`sha256.json` identifies both the raw and compressed bytes.

Regenerate in an empty working directory:

```sh
quartus_sh -t /path/to/fixtures/two-pll/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Quartus may choose different placement/routing across runs; the hashes identify
this preserved oracle rather than guaranteeing byte-identical rebuilds.
Compilation completed with zero errors and six warnings, including tied-reset
connectivity and the Lite LogicLock notice. No hardware was programmed.

The new second-site nextpnr profile is restricted to a 50 MHz reference.
25/100 MHz references at that site are not enabled or covered by this fixture.
