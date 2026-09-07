# triple225 Quartus phase reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
3 outputs at 50 MHz, phases 0/0/12500 ps, 50% duty,
direct integer mode. Host-only oracle; no hardware acceptance or deployment.
This reference frequency matches the DE10-Nano oscillator.

Output indices map to counters C6/C7/C5.
Fitter reports M12/N2, VCO 300 MHz, divide 6, counter presets
1/1/4, and phase mux presets 0/0/6.
All output high/low dividers are 3/3; odd-divider duty correction is off.
Default preset 1 and phase mux 0 are absent from non-default decomp text.

`top.v`, `oracle.tcl`, `pins.qsf`, and `clocks.sdc` retain portable build inputs.
`top.qsf` is exported with the source path made relative. Regenerate from an
empty working directory:

```sh
quartus_sh -t /path/to/phase-45/triple225/oracle.tcl
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
