# Quadrature Quartus reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
four 25 MHz outputs, direct integer mode, 50% duty, and phase shifts
0/10000/20000/30000 ps (0/90/180/270 degrees).
Host-only oracle; no hardware acceptance or deployment is claimed.

`top.v` clocks four seven-bit counters and connects their values, locked,
and three zero bits to the HPS GP input. `oracle.tcl`, `pins.qsf`, and
`clocks.sdc` preserve portable build inputs. `top.qsf` is the exported
assignment file with its absolute Verilog path made relative.
`top.rbf.gz` preserves the exact compiled RBF with gzip mtime zero;
`sha256.json` identifies the raw and compressed bytes.

Regenerate in an empty directory with Quartus 17.0.2 on PATH:

```sh
quartus_sh -t /path/to/fixtures/quadrature/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Placement/routing can vary across runs; hashes identify this oracle,
not a guarantee of byte-identical rebuilds. Counter placement uses the
same exact constraints as the existing four-output reference:

| Output | Counter | Fitter location | Phase | CNT_PRESET |
| --- | --- | --- | --- | --- |
| 0 | C6 | PLLOUTPUTCOUNTER_X0_Y20_N1 | 0 degrees | 1 (default) |
| 1 | C7 | PLLOUTPUTCOUNTER_X0_Y21_N1 | 90 degrees | 4 |
| 2 | C5 | PLLOUTPUTCOUNTER_X0_Y19_N1 | 180 degrees | 7 |
| 3 | C8 | PLLOUTPUTCOUNTER_X0_Y22_N1 | 270 degrees | 10 |

`fitter-pll.txt` preserves the complete PLL usage table: M12/N2, reported
300 MHz VCO, each counter divide12/high6/low6, 50% duty, and phase mux
preset zero. Decomp emits C7 preset hexadecimal04, C5 hexadecimal07,
and C8 hexadecimal0a. The zero-phase C6 preset is Mistral's default 1.
Analog settings are BWCTRL7, CP_CURRENT1 and LOCK_FILTER_CFG_SETTING
hexadecimal019.

`pll-settings.txt` contains every non-default FPLL/CMUX setting and
inversion plus their emitted routes, including tied-inactive reset and
the auxiliary bandgap powerdown on the other FPLL. Defaults are retained
in the complete RBF; full FPLL comparisons must load the RBF rather than
interpreting absent text fields as zero.

Compilation succeeded with zero errors and eight warnings: expected tied-reset
connectivity diagnostics, Lite LogicLock notice, and ignored GLOBAL_SIGNAL
assignments targeting the source clocks bus. The counter placement constraints
were honored; global output lanes are not all fixed by those assignments.
