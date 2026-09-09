# ref100-dual25 Quartus phase reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 100 MHz reference,
2 outputs at 25 MHz, phases 0/30000 ps, 50% duty,
direct integer mode. Host-only oracle; no hardware acceptance or deployment.
The DE10-Nano clock input supplies 50 MHz; non-50 MHz reference profiles
require an external reference and cannot be exercised with the current kit setup.

Output indices map to counters C6/C7.
Fitter reports M6/N2, VCO300 MHz, divide12, counter presets
1/10, and phase mux presets 0/0. Default preset1 and phase mux0
are absent from non-default decomp text.

`top.v`, `oracle.tcl`, `pins.qsf`, and `clocks.sdc` retain portable build inputs.
`top.qsf` is exported with the source path made relative. Regenerate from an
empty working directory:

```sh
quartus_sh -t /path/to/phase-rates/ref100-dual25/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

`top.rbf.gz` preserves the exact output with gzip mtime zero; `sha256.json`
records raw and compressed hashes. Rebuild placement/routing can vary.
`pll-settings.txt` preserves every non-default FPLL/CMUX setting and route;
full comparisons must load the RBF to include defaults. `fitter-pll.txt`
preserves the complete PLL usage section.

Compilation succeeded with zero errors. Expected warnings include tied reset,
the Lite LogicLock notice, and ignored GLOBAL_SIGNAL assignments on the source
bus. Counter placement is constrained; CMUX lane placement is not constrained.
