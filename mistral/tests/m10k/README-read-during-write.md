# M10K read-during-write contracts

`MISTRAL_M10K_TDP` exposes the Cyclone V true-dual-port read-during-write
contract at the nextpnr JSON boundary:

| Parameter | Scope | Default |
| --- | --- | --- |
| `CFG_RDW_MODE_A` | Read result when port A writes and reads the same word | `NEW_DATA_NO_NBE_READ` |
| `CFG_RDW_MODE_B` | Read result when port B writes and reads the same word | `NEW_DATA_NO_NBE_READ` |
| `CFG_RDW_MODE_MIXED` | A/B collision involving at least one write | `DONT_CARE` |

For the per-port parameters, the accepted contracts are
`NEW_DATA_NO_NBE_READ` (also `NEW_DATA` or `NEW`, numeric code `0`) and
`DONT_CARE` (also `DONTCARE`, numeric code `1`). The first is the Cyclone V
write-through behavior used by the existing M10K mapping. The second is a
weaker promise that is valid for an individual port. The mixed-port parameter
accepts only `DONT_CARE`, because cross-port collisions remain unspecified.
`OLD_DATA` (numeric code `2`) and `NEW_DATA_WITH_NBE_READ` (numeric code `3`)
are rejected because this device cannot implement those contracts for the
supported true-dual-port geometry.

There is no independent collision-mode bit in the Cyclone V M10K table.
Quartus Prime Lite accepts `DONT_CARE` for the mixed-port contract but emits
the same `TRUE_DUAL_PORT`, `A_DATA_FLOW_THRU` and `B_DATA_FLOW_THRU` settings
as `NEW_DATA_NO_NBE_READ`. The bitstream writer therefore keeps the existing
write-through data-flow settings; the new parameters prevent a requested
unsupported behavior from being silently discarded or replaced by a site
default. The normalized contracts are retained in `routed.json` for review.

These parameters apply only to `MISTRAL_M10K_TDP`. Supplying them to an SDP
`MISTRAL_M10K` is an error. The paired Yosys mapper currently emits the
write-through TDP form without these optional parameters, so nextpnr fills in
the defaults. A future mapper can pass through the contracts without changing
the physical M10K mapping.

Run the host regression with the locked tool builds:

```sh
python3 mistral/tests/m10k/collision.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/pins.qsf --sdc /path/to/clocks.sdc \
  --output /tmp/m10k-collision
```

The test checks default and explicit contracts, numeric-code normalization,
canonical routed JSON, decoded TDP flow-through settings, compressed RBF
generation, one M10K and 50 MHz timing. It also checks actionable rejection
of unsupported modes and use of the parameters on an SDP cell.
