# Mistral HeAP option regression

Run from the nextpnr source directory with an Intel ALM-capable Yosys and a
Mistral nextpnr executable:

```sh
python3 mistral/tests/placer_options.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --output /tmp/mistral-placer-options
```

The script synthesizes one small multiply/accumulate fixture, then performs
five actual placements with a fixed seed. It does not route, use a GPU, build
a bitstream or access hardware. It checks that:

- Mistral's beta 0.5 and criticality exponent 7 remain the defaults;
- explicit default values reproduce the default placement;
- changing either beta or exponent changes the placement;
- timing weight still changes placement, as a positive control;
- saved settings describe the values selected for placement.

Before the fix, `Arch::place` replaced beta and exponent after `PlacerHeapCfg`
read the settings. The override runs therefore reproduced the default
placement, while the saved default settings incorrectly reported the generic
beta 0.9 and exponent 2. Architecture defaults now enter the settings in the
constructor, before command-line overrides and generic fallback defaults.

The test compares placements within each run; it does not pin BEL names that
would prevent unrelated placer improvements. A changed placement proves an
option took effect, not that it improved timing.

Older saved JSON may record beta 0.9/exponent 2 even though placement used
0.5/7. Reproduce that placement from the original synthesis JSON, which has
no saved nextpnr settings, selecting
`--placer-heap-beta 0.5 --placer-heap-critexp 7` explicitly. The existing JSON
importer applies saved settings after command-line setup, so passing those
options while loading an old placed/routed JSON does not override stale
metadata. Correct or remove the two stale settings in a copy before loading
that form. This patch does not change JSON/CLI precedence.
