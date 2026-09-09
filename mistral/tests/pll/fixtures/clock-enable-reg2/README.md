# Double-register clock-enable oracle

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference on V11,
25 MHz integer PLL output, direct mode, zero phase and 50% duty. HPS GP output
bit 0 controls a `cyclonev_clkena` using `ena_register_mode="double register"`
and power-up low. The gated clock drives an eight-bit counter. The primitive's
`enaout` output is sampled by one fabric register on the 50 MHz board reference.

The double-register mode is the Cyclone V two-stage falling-edge enable path.
Quartus emits `ENABLE_REGISTER_MODE.2 REG2_ENOUT`; the power-up-low variant
emits `ENABLE_REGISTER_POWER_UP.2 0`. The high subdirectory contains the same
design with power-up high, for which Quartus uses the default value 1.

Locations are fixed at FPLL.000.014/C6 (`FRACTIONALPLL_X0_Y15_N0`,
`PLLOUTPUTCOUNTER_X0_Y20_N1`) and CMUXHG.000.035 lane 2 (`CLKCTRL_G2`). The
status route is `CMUXHG.000.035.2:SYN_EN` to fabric; direct HPS status routing
is not supported by the device fitter. Fitter excerpts and decompiled settings
are retained beside the exact compressed RBF. The RBF files are portable
Quartus outputs with gzip mtime zero; `sha256.json` covers both raw and
compressed bytes.

Regenerate in an empty output directory:

```sh
quartus_sh -t /absolute/path/to/fixtures/clock-enable-reg2/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

The fixture is a host-only configuration and routing oracle. It does not
establish hardware observation latency or provide an application CDC contract.
