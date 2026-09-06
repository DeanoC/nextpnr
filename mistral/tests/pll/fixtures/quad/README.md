# Four-output Quartus reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
25/50/100/75 MHz outputs, direct integer mode, zero phase, 50% duty.
Host-only oracle; no hardware acceptance or deployment is claimed.

`top.v` is the exact four-counter/HPS GP source used, identical to `../../quad.v`.
Each output clocks a seven-bit counter; the four counters, locked, and three
zero bits feed the 32-bit HPS GP input. `oracle.tcl`, `pins.qsf`, and
`clocks.sdc` retain the build inputs. `top.qsf` is the exported assignment
file with only its source-file absolute path made relative. `top.rbf.gz` is
the exact output compressed with gzip mtime zero; `sha256.json` identifies
the raw and compressed bytes.

Regenerate in an empty working directory, with `quartus_sh` on PATH:

```sh
quartus_sh -t /path/to/fixtures/quad/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

The Tcl locates source and pin assignments relative to itself. Quartus may
vary placement/routing across runs; preserved hashes identify this oracle,
not a promise of byte-identical rebuilds.

Counter placement is constrained: output0 uses C6 (divide12, high6/low6),
output1 C7 (divide6, high3/low3), output2 C5 (divide3, high2/low1,
odd-divider even-duty enabled), and output3 C8 (divide4, high2/low2).
C8's fitter location is `PLLOUTPUTCOUNTER_X0_Y22_N1`. Fitter reports M12/N2,
VCO300 MHz. Analog settings match the triple reference: BWCTRL7,
CP_CURRENT1, LOCK_FILTER_CFG_SETTING hexadecimal019.

C8 connects to `CMUXHG.000.035:PLLIN.12`; the oracle uses HG output2 with
INPUT_SEL hexadecimal14 (decimal20). C7 uses HG output3 select hexadecimal15
(decimal21); C6 uses `CMUXVG.042.000` output2 select hexadecimal11 (decimal17),
and C5 uses VG output0 select hexadecimal10 (decimal16). Counter placement
succeeded, but Quartus ignored GLOBAL_SIGNAL assignments targeting the source
`clocks` bus. Thus this fixture proves C8 and its horizontal input20 path,
without fixing all four horizontal output lanes.

`pll-settings.txt` contains all non-default FPLL and CMUX settings emitted by
Mistral decomp, including the tied-inactive reset. Defaults are represented
by the preserved complete RBF, so full setting comparisons must load that
RBF rather than treating absent text fields as zero. The other FPLL's
auxiliary powerdown entry is also retained.

Compilation completed with zero errors and eight warnings: expected tied-reset
connectivity diagnostics, Lite LogicLock notice, and ignored GLOBAL_SIGNAL
assignments described above. These are reference compilation results only.
