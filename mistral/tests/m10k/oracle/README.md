# M10K independent-clock configuration oracles

These two retained Quartus Prime Lite 17.0.2 Build 602 designs target
`5CSEBA6U23I7`. Each occupies one M10K in simple dual-port mode. They are
host-only configuration evidence, not hardware acceptance.

| Fixture | Configuration | Purpose |
| --- | --- | --- |
| `explicit20re1` | 512×20, separate write/read clocks, read enable tied high | Isolate read clock selection without a read-side ENABLE route |
| `dual40` | Initialized 256×40, separate clocks, conditional read | Show the 40-bit input-clock exception and read-enable routing |

Both use write `clka` on `CLKIN.0` and read `clkb` on `CLKIN.1`.
Quartus assigned clka to GCLK11 (`CMUXHG.089.035.3:CLKOUT`) and clkb to
GCLK10 (`CMUXHG.089.035.2:CLKOUT`). The following settings establish the
observed clock selection; omitted boolean/number fields have their Mistral
default zero value.

| Field | explicit20re1 | dual40 |
| --- | --- | --- |
| TOP_CLK_SEL | 1 | 1 |
| BOT_CLK_SEL | 1 | 1 |
| BOT_1_CORECLK_SEL | 1 | 1 |
| BOT_1_INCLK_SEL | 1 | 0 |
| BOT_1_OUTCLK_SEL | 1 | 1 |
| TOP_CLK_INV / BOT_CLK_INV | 0 / 0 | 0 / 0 |
| BOT_CORECLK_SEL | 0 | 1 |
| BOT_INCLK_SEL | 0 | 1 |
| BOT_CE0_SEL | 0 | 0 |

The 40-bit configuration uses both physical data input halves for writing.
Its `BOT_1_INCLK_SEL=0` keeps the second data input half on the write clock.
The read core still selects the independent read clock.

Read enable also requires configuration. In `dual40`, `re` routes to
`ENABLE.0`, with `BOT_CE0_SEL=0`, `BOT_CORECLK_SEL=1` and
`BOT_INCLK_SEL=1`. The generated RAM primitive gates both its read input
registers and read core with this enable. In `explicit20re1`, read enable
is constant high, there is no read-side ENABLE route, and those two clock
gating selectors remain zero. These are gating choices, not evidence of
a shared read/write clock. The nextpnr implementation uses the dynamic
ENABLE.0 arrangement for its logical B1EN in dual-clock mode.

Quartus also gates the write core using a routed write enable. Those
TOP_CORECLK/TOP_CE settings are recorded for completeness; the existing
nextpnr WREN-based write path must not acquire them without corresponding
ENABLE routing. Nor should physical write-pin choices be copied blindly:
nextpnr's narrow M10K cell has a historical active-low write-enable contract.

## Inspect the retained evidence

`top.rbf.gz` stores the original Quartus RBF with gzip compression to reduce
repository size. This is artifact storage compression, not FPGA configuration
compression. `mapping.json` contains the uncompressed SHA256 and byte count,
all TOP/BOT control settings including defaults, control routes, and relevant
generated primitive clock parameters.

From this directory, with `mistral-cv` available:

```sh
gzip -dc explicit20re1/top.rbf.gz > /tmp/m10k-explicit20.rbf
sha256sum /tmp/m10k-explicit20.rbf
mistral-cv decomp 5CSEBA6U23I7 /tmp/m10k-explicit20.rbf /tmp/m10k-explicit20.bt
mistral-cv routes 5CSEBA6U23I7 /tmp/m10k-explicit20.rbf
```

Compare the digest with `mapping.json`. Repeat for `dual40` to inspect the
width and enable differences. No local experiment RBF is required.

To regenerate either oracle, run `quartus_sh --flow compile top` inside
its directory. RTL and QSF use relative paths. The retained RBF is the
reference artifact; regeneration can change placement and routing while
preserving the relevant behavior and configuration relationships.

## True dual-port reference designs

`tdp10` and `tdp20` retain native BIDIR_DUAL_PORT Quartus designs with two
independently enabled read/write ports. Both use NEW_DATA_NO_NBE_READ on each
port and DONT_CARE for mixed-port collisions. Quartus rejected OLD_DATA for
this configuration. These are configuration oracles, not kit images.

Each directory includes relative-path source/QSF/QPF, `top.rbf.gz`,
`mapping.json` with its original digest and decoded settings, and
`port-mapping.json` correlating package signals with Mistral routes. Use the
same decompression, inspection and regeneration commands above. The TDP
backend's native 10/20-bit host outputs match every decoded non-RAM M10K
configuration setting in these oracles. See
[the TDP mapping](../README-true-dual-port.md) for logical port assignments.
