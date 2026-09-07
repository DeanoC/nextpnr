# ref25-triple400 Quartus reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 25 MHz reference,
25/50/100 MHz outputs and 25/50/25% duties, direct integer mode, zero phase.
Host-only reference; no hardware acceptance or deployment is claimed. The port
name `FPGA_CLK1_50` is retained for fixture compatibility, but the RTL reference
parameter and SDC period specify 25 MHz. A physical test requires that actual
reference frequency; the DE10-Nano onboard oscillator supplies 50 MHz.

The fitter confirms the requested reference, output frequencies, duties and
zero phases in `fitter-pll.txt`. M32/N2, reported VCO 400 MHz.
Fixed output counter placements: C6 divide 16 (high 4/low 12), C7 divide 8 (high 4/low 4), C5 divide 4 (high 1/low 3).
The checked feedback tuple uses BWCTRL 6, CP_CURRENT 1, M low preset 1,
and M phase preset 0. Full FPLL values, including defaults, must be compared
using the complete RBF; `pll-settings.txt` contains non-default FPLL and CMUX
settings and routes. Missing text settings are defaults, not necessarily zero.

`top.v`, `oracle.tcl`, `pins.qsf`, and `clocks.sdc` are portable inputs.
`top.qsf` is the exported assignment file with its source path made relative.
`top.rbf.gz` preserves the complete RBF with gzip mtime zero; `sha256.json`
identifies raw and compressed bytes. Mistral decomp used commit
`78ba2a580ae2523403d4f4f91891a6b11d7b6aba`.

Regenerate in an empty working directory:

```sh
quartus_sh -t /path/to/fixtures/multi-reference/ref25-triple400/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Quartus can vary placement and routing; hashes identify the preserved reference,
not a byte-identical rebuild promise. Compilation succeeded. Warnings concern
tied-reset connectivity, Lite LogicLock support and ignored GLOBAL_SIGNAL
assignments to the source clock bus. Counter locations were honored; CMUX lanes
are not constrained. OSS tests compare every FPLL setting and check OSS clock
selectors separately.
