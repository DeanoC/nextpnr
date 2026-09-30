# Experimental Mistral control remapping

These opt-in passes explore equivalent control logic after synthesis and
placement. They use bounded Boolean proofs and the existing Mistral timing
model. They are disabled by default and do not change the selected compiler
recipe, RTL or clock constraints.

| Operation | Explicit request | Detailed guide |
| --- | --- | --- |
| Unplaced pure-cube balancing | Repeat `--balance-reduction-root CELL` | [Reduction balancing](../mistral/reduction_balance.md) |
| Local FF-enable composition | `--remap-critical REPORT`, `--remap-candidate N`, `--remap-groups N` | [Local path remapping](local-path-remap.md) |
| Local input ordering / fixed FF placement | `--remap-optimize-pins`, `--remap-preserve-ffs` with a local request | [Staged local remapping](../mistral/remap_plan.md) |
| One to eight local stages | `--remap-plan FILE` | [Local plan schema](../mistral/remap_plan.md) |
| Bounded two/three-LUT internal cut | `--remap-comb-critical REPORT`, `--remap-comb-candidate N`, or `--remap-comb-plan FILE` | [Internal-cut plans](../mistral/comb_plan.md) |
| Hard-IP capture locality | `NEXTPNR_MISTRAL_CAPTURE_LOCALITY="REPORT BUDGET RADIUS"` | [Capture locality](mistral-capture-locality.md) |
| Placed eleven/24-literal balancing | `NEXTPNR_MISTRAL_PLACED_REDUCTION="ROOT RADIUS SELECTION"` (one to eight lines) | [Placed balancing](../mistral/reduction_balance.md) |
| Four-LUT, seven-essential-input decomposition | `--remap-decompose-critical REPORT`, `--remap-decompose-candidate N` | [Control decomposition](../mistral/control_decomposition.md) |

Unplaced balancing runs during packing. After ordinary HeAP placement, the
order is local remapping, internal-cut remapping, capture locality, placed
reduction stages and control decomposition, then routing. A plan replaces its
corresponding single-request form. Relevant report edges, cells, locations and
clock attribution must match the live graph at the stage where they are used.
Keep authenticated reports and input hashes; a report is not a saved Context
or a complete design-equivalence certificate.

Local/comb plans and decomposition require fresh packing and ordinary HeAP
placement. Their final listing requires `--no-route` and no `--rbf`; all prior
stages must select a candidate. A failed selected stage aborts before routing,
including with `--force`. Use `--no-route` and omit `--rbf` for standalone
diagnostic listing too; the legacy interfaces do not all enforce the newer
plan listing guards. A listed placement-model gain requires a separate full
route before making a routed timing claim.

## Routed RAM-test measurement

The frozen groups-eight profile used compiler
`6ad210a60991f93a607ac4490d6282b1267ec64a`, the same RTL, synthesis JSON,
QSF/SDC, BUILD_ID, seed 2 and GPU device 1 as the retained parent116 control.
It retained the existing unplaced reduction root, three selected local
stages, internal-cut candidate 0, capture locality and placed eleven-literal
candidate 0, then selected control-decomposition candidate 0.

The local plan used group budgets **8, 1, 1**, candidate 0 for each stage,
`optimize_pins: false` and `preserve_ff_placement: true`. Capture locality used
budget 64 and radius 24 and retained 60 moves. Placed eleven-literal balancing
used radius 6 and selection 0. Each stage used its authenticated report role
from the retained profile; this is a complete experimental profile, not an
isolated attribution of the gain to one pass. The 24-literal placed rewrite
and optional local pin ordering were not selected in this measured route.

| Clock | Parent116 Fmax MHz | Groups-eight Fmax MHz | Required MHz |
| --- | ---: | ---: | ---: |
| Memory | 116.171005 | 116.279068 | 130.005203 |
| Pixel | 93.668045 | 93.896713 | 74.250069 |
| Capture | 378.582397 | 307.631805 | 130.005203 |

Memory improves by **0.108063 MHz (0.0930%)**. Pixel improves by 0.228668 MHz.
Capture loses 70.950592 MHz of achieved Fmax, while continuing to meet its
required clock. The reported capture transfer has a 5 ns setup window;
its positive headroom decreases from 3.283 ns to 2.887 ns. The route completes
and has no final reported hold violations. It still fails the 130 MHz memory
target.

The publication acceptance rule is a positive target-memory gain, passing
non-target clock constraints and final holds, and a successful full routed
graph, physical-pin and buffer proof. Lower positive non-target headroom is
disclosed rather than automatically rejected. This is separate from the
earlier diagnostic that required every achieved clock Fmax to be nonregressing:
that stored parent116 strict result remains **false**. This measurement is a
small target-clock gain with a capture-headroom tradeoff, not all-clock Fmax
nonregression or 130 MHz timing closure.

The full comparison independently proves the actual enlarged generated
ready-copy LUT, its selected FF-enable consumers and declared placement
change. It preserves synthesis-original physical pins and BELs, routing-created
buffers and connections outside the declared changes. It also proves all
128 rows of the actual seven-input control decomposition. Preservation does
not mean that every previously generated ready-copy LUT retains its old BEL.

The measured compiler binary SHA256 is
`ba39ad814af51c4a6af3ca1854fcd1dd992af350bb09741d5b6becfa2d7d4b18`.
The frozen full graph-proof record SHA256 is
`3fe9d625951ec8d9b9d8034e279d456f82804106761854cefca42a1495830e1e`.
The separate publication acceptance record authenticates 162 retained
artifacts and preserves that proof's strict-result flag. Its SHA256 is
`c70363931e903723bff55522f2fca2eb62dac375f7387c58f26ab7619e903483`. The generated RBF
SHA256 is `2eca0c165601511c391d7072d3a7386369bbdd2530bb13eab288d005c0806421`.

This is host-side modeled timing and full routing evidence, without hardware
acceptance. It does not select a default recipe or compiler lock. Later
documentation commits do not change the measured source identity. The newer
main-branch locked-LAB legalization change was not included in this benchmark.
