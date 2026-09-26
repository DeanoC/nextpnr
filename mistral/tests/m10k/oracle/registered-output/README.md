# Registered M10K output oracles

These Quartus Prime Lite 17.0.2 Build 602 compilations target
`5CSEBA6U23I7` and isolate the M10K output-register selectors. They are
host-only configuration evidence; they are not hardware acceptance images.

| Fixture | Quartus mode | Configuration | Decoded output selectors |
| --- | --- | --- | --- |
| [`tdp20-b`](tdp20-b) | `BIDIR_DUAL_PORT` | 512×20, B output registered on `CLOCK1` | `B_OUTPUT_SEL=REG` |
| [`sdp40-b`](sdp40-b) | `DUAL_PORT` | 256×40, B output registered on `CLOCK1` | `A_OUTPUT_SEL=REG`, `B_OUTPUT_SEL=REG` |

The 40-bit simple-dual result occupies both physical output halves. Quartus
therefore selects both registers even though the logical primitive has only
port B output data. The corresponding nextpnr parameter is
`CFG_OUT_REG_B=1`; the backend mirrors it to the A half.

Each directory keeps the relative-path RTL/QSF/QPF and a compressed copy of
the generated RBF. `mapping.json` records the uncompressed digest, placement,
decoded M10K settings and representative control routes. Inspect an artifact
with:

```sh
gzip -dc tdp20-b/top.rbf.gz > /tmp/m10k-registered.rbf
sha256sum /tmp/m10k-registered.rbf
mistral-cv decomp 5CSEBA6U23I7 /tmp/m10k-registered.rbf /tmp/m10k-registered.bt
```

Regenerating with Quartus can choose a different site or routing while
preserving the selector relationship.
