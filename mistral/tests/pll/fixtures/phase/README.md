# Checked Quartus phase references

These are complete Quartus Prime Lite 17.0.2 RBF references for Cyclone V
5CSEBA6U23I7, generated on 2026-09-06. No hardware acceptance is implied.
Each profile directory contains the exact RTL, pin assignments and Tcl used,
all non-default selected `FPLL.000.014` settings, the gzip-compressed RBF,
and SHA256 checksums for both raw and compressed bytes.

Reproduce either reference in an empty directory using the profile's saved Tcl:

```sh
quartus_sh -t /absolute/path/to/180/phase-oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Use `270/phase-oracle.tcl` for 270 degrees. The inputs retain their original
`phase90` signal name; the `phase_shift1` value determines the actual phase.
`quartus.rbf.gz` uses gzip timestamp zero. The repository's `phase.py --degrees
180` or `--degrees 270` automatically verifies both checksums, decompresses
and decompiles the bundled reference, and compares all selected FPLL settings
before testing OSS output. Quartus is unnecessary for running these checks.

Both profiles use M12/N2/C12/C12, a 50 MHz reference, 25 MHz outputs, and
50% duty. Compared with the established 90-degree profile, the only selected
FPLL change is `CNT_PRESET.7`: `04` becomes `07` at 180 degrees or `0a` at
270 degrees. Quartus STA also reports the requested 180.00/270.00-degree phases.
