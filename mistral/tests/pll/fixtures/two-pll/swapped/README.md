# Swapped integer/fractional two-PLL reference

This is the companion to the parent two-PLL fixture. It exchanges the output
profiles while preserving instance names, observation logic and forced sites:
`video_pll` at FPLL.000.014 now generates fractional 12.288 MHz, and
`audio_pll` at FPLL.000.031 generates integer 25 MHz. Both use C6 with zero
phase and 50% duty from the same 50 MHz V11 reference. This proves each
feedback profile at each site when compared with the parent fixture.

Quartus Prime Lite 17.0.2 completed with zero errors and six warnings.
The reference selects remain 4 at FPLL.000.014 and 6 at FPLL.000.031.
Video uses VG42,0 lane3 selector `11` hexadecimal; audio uses HG0,35 lane0
selector `0e` hexadecimal and `PRE_SYNENB`. Auxiliary bandgap powerdown is
emitted only at FPLL.000.073.

Regenerate in an empty working directory:

```sh
quartus_sh -t /path/to/fixtures/two-pll/swapped/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

The bundled source, Tcl, pin and clock constraints are the build inputs;
`top.qsf` makes the exported source path relative. The fitter PLL report and
all emitted FPLL/CMUX settings are retained. `top.rbf.gz` preserves the complete
RBF with gzip mtime zero, and `sha256.json` identifies raw and compressed bytes.
Load that RBF for complete comparisons because omitted text settings are defaults.
Hashes identify this oracle, not a guarantee of byte-identical Quartus rebuilds.

Host-only; no hardware was programmed. The new second-site nextpnr profile
is restricted to the 50 MHz reference; 25/100 MHz references are not enabled.
