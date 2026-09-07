# tripleLate Quartus phase reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
3 outputs at 25 MHz, phases 0/0/270 degrees,
50% duty, direct integer mode. Host-only oracle; no hardware acceptance or deployment.

Output indices map to counters C6/C7/C5.
Fitter reports M12/N2, VCO300 MHz, divide12 (high6/low6), phase mux0,
and presets 1/1/10. Zero-phase preset1 is the bitstream default
and is therefore absent from non-default decomp text. Nonzero presets are
4 for 90 degrees, 7 for 180 degrees, and 10 for 270 degrees.

`top.v`, `oracle.tcl`, `pins.qsf`, and `clocks.sdc` retain portable build inputs.
`top.qsf` is exported with the source path made relative. Regenerate from an
empty working directory:

```sh
quartus_sh -t /path/to/phase-select/tripleLate/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

`top.rbf.gz` preserves the exact output with gzip mtime zero; `sha256.json`
records raw and compressed hashes. Rebuild placement/routing can vary.
`pll-settings.txt` preserves every non-default FPLL/CMUX setting and route;
full comparisons must load the RBF to include defaults. `fitter-pll.txt`
preserves the complete PLL usage section.

Compilation succeeded with zero errors and 7 warnings (tied reset,
Lite LogicLock notice, ignored GLOBAL_SIGNAL assignments on the source bus).
Counter placement is constrained; CMUX lane placement is not constrained.
