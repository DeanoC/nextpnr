# Fabric-controlled PLL clock enable

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference on V11,
25 MHz integer PLL output, direct mode, zero phase and 50% duty. HPS GP output
bit 0 controls a `cyclonev_clkena` primitive. The gated output clocks an eight-bit
counter observed on HPS GP input bits 7:0; bit 8 observes PLL lock.
This is a host-only configuration oracle; no hardware was programmed.

The primitive explicitly requests `clock_type="global clock"`,
`ena_register_mode="falling edge"`, `ena_register_power_up="high"`,
`disable_mode="low"` and `test_syn="high"`. `enaout` is unused. The enable
is registered on the falling clock edge, and the disabled output stays low.
The enable register starts high. This fixture does not establish hardware
stop/resume behavior or cover other register modes, power-up values or enaout.

Locations are constrained to `FRACTIONALPLL_X0_Y15_N0` (FPLL.000.014),
`PLLOUTPUTCOUNTER_X0_Y20_N1` (C6, divide 12) and `CLKCTRL_G2`
(CMUXHG.000.035 lane 2). Quartus uses M12/N2 and a 300 MHz VCO.
The clock mux uses INPUT_SEL `16` hex and TESTSYN_ENOUT_SELECT `PRE_SYNENB`,
as for the existing ungated PLL path. The enabled path changes
ENABLE_REGISTER_MODE to `REG1_ENOUT` and routes the fabric signal to
`CMUXHG.000.035.2:ENABLE` via `TD.000.036.0099`. ENABLE_REGISTER_POWER_UP
is absent from the decomposition because its Mistral default is 1.

`top.v`, `oracle.tcl`, `pins.qsf` and `clocks.sdc` are build inputs. `top.qsf`
is the exported assignments with the source path made relative. The fitter
excerpt preserves PLL and global clock usage. `pll-settings.txt` preserves
all emitted FPLL/CMUX settings plus the enable endpoint route. Omitted
settings have Mistral defaults, not necessarily zero. `top.rbf.gz` contains
the exact raw Quartus RBF, compressed with gzip mtime zero; `sha256.json`
identifies both forms. Complete setting comparisons should load the RBF.

Regenerate in an empty output directory:

```sh
quartus_sh -t /path/to/fixtures/clock-enable/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Compilation completed with zero errors and four warnings (including the
unconnected PLL reset warning and Lite LogicLock notice). Placement/routing
may change across runs; hashes identify the preserved output, not a promise
of byte-identical rebuilding. Mistral decoder commit:
`78ba2a580ae2523403d4f4f91891a6b11d7b6aba`.
