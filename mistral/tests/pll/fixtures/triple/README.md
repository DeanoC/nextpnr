# Three-output Quartus reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
25/50/100 MHz outputs, direct integer mode, zero phase, 50% duty.
Host-only oracle; no hardware acceptance or deployment is claimed.

`top.v` is the exact three-counter/HPS GP source used. `oracle.tcl`,
`pins.qsf`, and `clocks.sdc` retain the build inputs. `top.qsf` is the
exported assignment file with only its source-file absolute path made relative.
`top.rbf.gz` is the exact output, compressed with gzip mtime zero;
`sha256.json` identifies the raw and compressed bytes.

Regenerate in an empty working directory, with `quartus_sh` on PATH:

```sh
quartus_sh -t /path/to/fixtures/triple/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

The Tcl locates the source and pin assignments relative to itself. Quartus
may vary placement/routing across runs; preserved hashes identify this oracle,
not a promise of byte-identical rebuilds.

Counter placement is constrained: output0 uses C6 (divide12, high6/low6),
output1 C7 (divide6, high3/low3), output2 C5 (divide3, high2/low1,
odd-divider even-duty enabled). Fitter reports M12/N2, VCO300 MHz.
C5 connects to `CMUXHG.000.035:PLLIN.15`; the oracle uses HG output2
with INPUT_SEL hexadecimal17 (decimal23). C7 uses HG output1 select
hexadecimal15 (decimal21); C6 uses `CMUXVG.042.000` output2 select
hexadecimal11 (decimal17). Counter placement succeeded, but Quartus ignored
the GLOBAL_SIGNAL assignments targeting the source `clocks` bus. Thus this
fixture proves C5 and its horizontal input23 path, without fixing all three
horizontal output lanes.

`pll-settings.txt` contains all non-default FPLL and CMUX settings emitted by
Mistral decomp, including the tied-inactive reset. Defaults are represented by
the preserved complete RBF, so full setting comparisons must load that RBF
rather than treating absent text fields as zero. The other FPLL's auxiliary
powerdown entry is also retained.

Compilation completed with zero errors and seven warnings: expected tied-reset
connectivity diagnostics, Lite LogicLock notice, and ignored GLOBAL_SIGNAL
assignments described above. These are reference compilation results only.
