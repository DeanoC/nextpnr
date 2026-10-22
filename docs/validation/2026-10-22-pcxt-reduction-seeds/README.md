# PC/XT reduction recipes across three seeds

These are host-only fresh raw-synthesis pack/place/route diagnostics using frozen compiler `cbb1e1f1`, binary `6834b09bc3ccc63105d3027db0c0415a6dc080cc923645126c3a0fca4ec65fde`. CPU/system and microcode remain 100 MHz, pixel 74.25 MHz. Each recipe keeps timing weight 2000 and criticality exponent 5. The sibling balanced-reduction capture supplies seed 1 and raw inputs; this capture retains the six additional complete routes and exact receipts. All runs use shared GPU phase leases and native search bounds. No hardware ran.

| Recipe | Seed | System WNS, ns | Microcode return WNS, ns | Pixel WNS, ns | Setup failures |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline | 1 | -1.602 | +0.001 | +0.675 | 369 |
| baseline | 2 | -1.676 | +0.110 | -1.223 | 160 |
| baseline | 3 | -1.747 | +1.323 | -0.383 | 231 |
| overflow17 | 1 | -1.232 | +0.397 | +0.370 | 150 |
| overflow17 | 2 | -1.437 | +0.247 | +0.269 | 274 |
| overflow17 | 3 | -1.201 | +0.106 | -0.564 | 89 |
| both | 1 | -1.349 | -0.046 | -0.664 | 318 |
| both | 2 | -1.252 | +0.445 | -0.146 | 216 |
| both | 3 | -1.328 | +0.625 | +0.503 | 289 |

All nine runs fail setup. Every run has zero reported related-hold failures and 105 unsupported boundaries. No recipe passes the unchanged two-of-three gate. Clock-summary system WNS can include outgoing CPU-to-ROM paths; analysis.json retains every launch/capture pair separately. Positive margins do not establish complete IO/clock/reset coverage.

The six new snapshots each retain 17,199 endpoint identities/rows. Within each paired seed, complete timed identities, clock constraints, setup windows and supported/unsupported identities match. Full static, constant and unsupported detail records are compared explicitly and may differ with placement; no intersection or shortened inventory is used. Per-endpoint margin regressions remain recorded. This is not a complete independent final routed graph or physical RBF equivalence proof; the sibling capture retains complete unpacked Boolean proofs and explains the outstanding route-through ancestry work.

Every receipt artifact hash is rechecked by analyze.py. Original producer inputs are identified in input-manifest.json and retained in the sibling 2026-10-22-pcxt-balanced-reductions capture. Captured absolute paths must be adjusted to extracted inputs and a rebuilt compiler of the recorded identity.

The six runs charge 1838.67921122303227 seconds. Added to 10575.23023688094620 seconds through the earlier seed 1 comparison, their subtotal is 12413.90944810397847 seconds. Concurrent RAM-decoder and spreader routes are excluded and recorded separately. The user removed the total route cap; failed runs remain charged and retained.

No production RTL, clock, recipe, lock, seal or shared contract changes. Draft PRs and FES #840 remain open. Further compiler/placement work, complete coverage, a passing same-recipe seed gate, a fresh producer and exact-artifact hardware qualification remain required.

Archive SHA256: `d72ceb460c275cfcfa5d8407198a5e19f6ab2973d5c4f201122f348faba4e9eb`; all 52 members pass size and SHA256 readback.
