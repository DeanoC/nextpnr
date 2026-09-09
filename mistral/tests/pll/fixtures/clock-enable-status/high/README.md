# Registered clock-enable status output, power-up high

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference on V11,
25 MHz integer PLL output, direct mode, zero phase and 50% duty. HPS GP output
bit 0 controls a falling-edge `cyclonev_clkena` with power-up high. Its gated
clock drives an eight-bit counter. The primitive's `enaout` output is sampled
by one fabric register on the 50 MHz board reference. HPS GP input bits 7:0
observe the counter, bit 8 PLL lock and bit 9 the sampled status.

Locations are fixed at FPLL.000.014/C6 (`FRACTIONALPLL_X0_Y15_N0`,
`PLLOUTPUTCOUNTER_X0_Y20_N1`) and CMUXHG.000.035 lane 2 (`CLKCTRL_G2`).
The actual status route is `CMUXHG.000.035.2:SYN_EN` to `V4.000.032.0005`.
Quartus retains the physical clock-enable output; it does not replace it
with a fabric enable register or alias the unregistered `ena` input.
The sampled status register is explicitly present in the RTL.

Clock-enable settings remain `REG1_ENOUT`, power-up 1 (omitted/default), INPUT_SEL `16` and
TESTSYN_ENOUT_SELECT `PRE_SYNENB`. There is no emitted SYN_EN inversion.
The extra board-clock buffer appears in the complete CMUX settings.
This is a host-only configuration and routing oracle; it does not establish
hardware observation latency or provide a CDC synchronizer for application use.

An initial direct `enaout` to HPS GP bit 9 connection failed Quartus fitter
with Error 175006: "Could not find path between source global clock driver
and the HPS_INTERFACE_MPU_GENERAL_PURPOSE". Sampling the output in fabric
allows the fixed HG site to route. The fixture therefore exercises a fabric
sink, not a direct periphery connection.

`top.v`, `oracle.tcl`, `pins.qsf` and `clocks.sdc` are build inputs. `top.qsf`
is the exported assignments with its source path made relative. Fitter excerpts
preserve PLL and clock usage. `pll-settings.txt` contains emitted FPLL/CMUX
settings, reset inversions and enable/status endpoint routes; omitted settings
have Mistral defaults. The exact Quartus RBF is in `top.rbf.gz` (gzip mtime 0),
with raw and compressed hashes in `sha256.json`.

Regenerate in an empty output directory:

```sh
quartus_sh -t /path/to/fixtures/clock-enable-status/high/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Compilation passed with zero errors and four warnings, including tied PLL
reset and Lite LogicLock notices. Hashes identify the preserved output;
placement and routing may vary on rebuilding. No hardware was programmed.
Mistral decoder commit: `78ba2a580ae2523403d4f4f91891a6b11d7b6aba`.
