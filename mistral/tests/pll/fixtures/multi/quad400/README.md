# 25/50/100/80 MHz Quartus reference

Quartus Prime Lite 17.0.2, device `5CSEBA6U23I7`, 50 MHz reference,
25/50/100/80 MHz outputs, direct integer mode, zero phase, 50% duty.
Host-only oracle; no hardware acceptance or deployment is claimed.

`top.v` preserves the existing quad fixture's counters and HPS GP wiring;
only output frequency strings change. `oracle.tcl`, `pins.qsf`, and
`clocks.sdc` are the exact portable build inputs. `top.qsf` is the exported
assignment file with its source path made relative. `top.rbf.gz` contains
the exact RBF compressed with gzip mtime zero; `sha256.json` identifies
both raw and compressed bytes.

Regenerate in an empty working directory:

```sh
quartus_sh -t /path/to/fixtures/multi/quad400/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Counter placement remains constrained to C6 divide16 (8/8), C7 divide8 (4/4), C5 divide4 (2/2), C8 divide5 (3/2, odd-divider even-duty enabled).
The complete feedback/analog tuple matches `checked_configs(50)`:
M16/N2, bandwidth7, charge pump1, M low preset1, M phase preset0. Lock filter is hexadecimal019.
Quartus ignores GLOBAL_SIGNAL assignments on the source clock bus, so
this fixture fixes PLL counters without requiring the same CMUX output lanes
as nextpnr.

`pll-settings.txt` preserves every non-default FPLL and CMUX setting, including
the auxiliary FPLL powerdown. Compare complete decompressed RBF settings;
absent fields represent device defaults, not zero. Placement/routing can
vary on regeneration; hashes identify this captured build.

Full compilation: 0 errors, 8 warnings.
