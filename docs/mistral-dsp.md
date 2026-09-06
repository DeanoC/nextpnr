# Cyclone V 9x9 DSP support

The Mistral architecture exposes one `MISTRAL_MUL9X9` BEL per physical DSP
block. `5CSEBA6U23I7` has 112 such BELs. A placed multiplier reserves the whole
block; packing three independent 9x9 multipliers into its shared mode and
register controls is not implemented. Other multiply sizes are not supported
by this change.

`mistral/dsp.cc` imports `CycloneV::dsp_get_pos()`. Lane 0 connects `A[8:0]`
to DSP `DATAIN` group 0, `B[8:0]` to group 2, and `Y[17:0]` to
`RESULT[17:0]`. These are Mistral's existing ports and routing nodes, as
documented in its `docs/cyclonev_details.rst`; no routing or configuration
tables are added. BEL and cell names match, so the existing placer legality,
BEL buckets and default pin mapping apply without cell renaming.

The existing packer's constant/inverter folding is enabled through
`mistral/pins.cc`. DSP inputs float high and have per-bit inversion controls.
`mistral/bitstream.cc` selects `M9X9`, writes `AX_SIGNED`/`AY_SIGNED` from the
Yosys parameters (default true), and writes all twelve `DATA_INV` groups.
Unused inputs are zeroed. Mistral's cleared configuration bypasses input and
output registers and disables preaddition and cascade. No FFs are packed into
the DSP. Configuration API calls are checked for success.

`mistral/delay.cc` classifies A/B/Y as combinational ports and uses the Cyclone V
arcs in locked Yosys `techlibs/intel_alm/common/dsp_sim.v`: A→Y 2818 ps and
B→Y 3051 ps. These fixed arcs retain the source model's speed-grade limitations.
The synchronous clock Fmax does not constrain asynchronous HPS GP operands.

## Source bases

This implementation starts from these exact revisions:

| Repository | Base |
| --- | --- |
| nextpnr | `7d4f72c0aabc15da932748a54e82a6ff7b41921e` |
| Mistral | `bfa096c1deac6180a3eee784693c28dac491ab18` |
| Yosys | `13b43f8c85ec430a33ee55d058fb4c32b42b6910` |
| misteross regression | `2d171c8f6e9ef55ef4b34a7fe035aa176c30144b` |

Mistral also needs the accompanying `add_cram_blocks()` upper-tile fix for
complete reverse port lookup at DSPs ending a BEL span. It marks the upper
tile `T_DSP2` when inserting the base, without adding a physical block or
changing configuration tables. Its `tests/dsp-ports.cc` checks all data/result
port round trips, including those boundary sites.

## Host regression

Regenerate `build/oss/060_dsp_mul/synth.json` with locked Yosys in a separate,
clean misteross checkout. The original tools must fail placement with no
`MISTRAL_MUL9X9` BEL. Build this nextpnr with the accompanying Mistral source:

```sh
cmake -S "$NEXTPNR_SOURCE" -B "$DSP_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DARCH=mistral -DMISTRAL_ROOT="$MISTRAL_SOURCE" \
  -DBUILD_PYTHON=OFF -DBUILD_GUI=OFF -DBUILD_TESTS=OFF -DUSE_IPO=OFF
cmake --build "$DSP_BUILD" --target nextpnr-mistral -j8
python3 "$NEXTPNR_SOURCE/mistral/tests/mul9x9.py" \
  --nextpnr "$DSP_BUILD/nextpnr-mistral" --mistral-cv "$MISTRAL_CV" \
  --fixture "$MISTEROSS/build/oss/060_dsp_mul/synth.json" \
  --qsf "$MISTEROSS/boards/de10nano/pins.qsf" \
  --sdc "$MISTEROSS/boards/de10nano/clocks.sdc" --output "$DSP_RESULTS"
```

Use `mistral-cv` built with the upper-tile fix. The regression routes the original
fixture plus explicit unsigned/constant-high and mixed-signed/input-inversion
variants in separate output directories. It requires one DSP, one HPS GP,
zero M10K and a passing intended 50 MHz clock. It decodes each compressed RBF
to check mode, signedness, constant/inversion masks, bypass and port routes.
This is host configuration evidence, not an arithmetic hardware test.

At the misteross base above, real `make oss` builds require canonical local
tool directories plus binary digests and commit stamps matching `toolchain.lock`.
The advertised install override is accepted only for command printing, not real
builds. Do not rewrite those stamps to make an uncommitted tool appear pinned.
For development, run the same nextpnr command directly, as the regression does.
After review and authorized tool commits, update the nextpnr and Mistral lock
entries, bootstrap them normally, then run `make oss EXP=060_dsp_mul`.
