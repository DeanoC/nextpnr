# triple400 asymmetric-duty Quartus reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
25/50/100 MHz outputs and 25/50/25% duty, direct integer mode, zero phase.
Host-only reference; no hardware acceptance or deployment is claimed.

The fitter confirms every requested frequency, duty and zero phase exactly.
M16/N2, VCO400 MHz. Fixed counter placements: output0: C6, divide16, high4/low12, output1: C7, divide8, high4/low4, output2: C5, divide4, high1/low3.
All odd-divider even-duty enables are off. Full non-counter FPLL settings
match the existing `../../multi/triple400` oracle, including analog and feedback
preset settings. BWCTRL is 7, CP_CURRENT is 1,
and LOCK_FILTER_CFG_SETTING is hexadecimal019. CP_CURRENT zero is a default
and therefore omitted from decomp text.

`top.v`, `oracle.tcl`, `pins.qsf`, and `clocks.sdc` retain portable inputs.
`top.qsf` is the exported assignment file with the source path made relative.
`top.rbf.gz` preserves the complete RBF using gzip mtime zero; `sha256.json`
identifies both raw and compressed bytes. `pll-settings.txt` retains every
non-default FPLL and CMUX setting and route, including tied-inactive reset.
Load the complete RBF for full setting comparisons: absent text settings
are defaults, not necessarily zero.

Regenerate in an empty working directory:

```sh
quartus_sh -t /path/to/fixtures/multi-duty/triple400/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Quartus may vary placement/routing; hashes identify the preserved reference,
not a byte-identical rebuild promise. Compilation succeeded with zero errors
and 7 warnings (tied-reset connectivity, Lite LogicLock notice and ignored
GLOBAL_SIGNAL assignments to the source clock bus). Counter locations were
honored; CMUX output lanes are not constrained.
