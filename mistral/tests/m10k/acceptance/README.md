# Hardware acceptance, 2026-09-08

These are the exact OSS RBFs checked on the designated DE10-Nano kit under
owner `nextpnr-m10k-dual` using the kit.py lease/load/stop protocol. Each
passed all three checks recorded in `results.json`: initialized full-width
data, writes with the read clock stopped followed by resume, and read/write
enable behavior. The test uses a 50 MHz board reference, 50 MHz writes and
an independently gated 25 MHz read clock. No non-50 MHz reference was used.

| Width | Write-clock reported Fmax | Constraint |
| --- | --- | --- |
| 20 | 427.533 MHz | 50 MHz |
| 40 | 321.543 MHz | 50 MHz |

Both use one M10K, one PLL and one HPS GP interface, with no DSP blocks.
The timing report also identifies read output paths as originating from the
25 MHz read clock. This is settled-data host probing, not CDC timing acceptance.

Build provenance:

- nextpnr base: `9632c85b84069acc8bb507165a48c348c70499eb`, plus the changes in this PR.
- Yosys techlibs: `eeb2543e2d482453d3bdc331462c57437646ff43`, based on `1e7fbaee2fa3e1fc2f68199bebd413061a4628fb`.
- Yosys executable: locked `13b43f8c85ec430a33ee55d058fb4c32b42b6910`; the changes are runtime Verilog/mapping files, supplied through an isolated installation.
- Mistral: `78ba2a580ae2523403d4f4f91891a6b11d7b6aba`, unchanged.
- Device: `5CSEBA6U23I7`; board QSF/SDC: misteross `ffdac202fd52e06855afb7d2baa8d4946ff765db`, `boards/de10nano`.

The source is `../dual_clock.v` with WIDTH20/40. Both artifacts were generated
with `synth_intel_alm -nolutram -nodsp -top top` and nextpnr
`--device 5CSEBA6U23I7 --compress-rbf`. The 20-bit acceptance build used the
source default parameter; the portable regression explicitly sets WIDTH for
both cases, so its independently regenerated 20-bit artifact can differ.
`results.json` records hashes of the uncompressed files and their actual
post-route utilization/timing summaries.

Extract an artifact with `gzip -dc top20.rbf.gz > /tmp/top20.rbf`, verify its
SHA256 against `results.json`, then load it through an authorized kit session
and run `../probe.sh 20` on the target. Use the corresponding files and width
for 40. Stop through kit.py afterward; its development reboot recovery returns
the kit to the free state. These instructions do not bypass lease ownership.
