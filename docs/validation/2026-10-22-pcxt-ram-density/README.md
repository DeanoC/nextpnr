# PC/XT global HeAP density diagnostics, 2026-10-22

None of these four diagnostics qualifies for timing acceptance. Increasing the
global HeAP beta to 0.75 or 0.90 did not bring the microcode ROMs into the nearer
RAM columns. One job failed routing; the other three emitted complete final
artifacts and failed setup timing. No hardware ran, and no clocks, constraints,
RTL, keep guards, module contracts or production locks changed in this experiment.

## Frozen comparison

All runs use seed 1, 100 MHz, timing weight 2000 and criticality exponent 5, with
the same synthesized input, QSF and SDC. Both baseline and `overflow17` families
enable `--placer-heap-correct-spread`. Each density route command differs from
its corresponding beta-0.5 reference only by `--placer-heap-beta 0.75` or `0.90`.
The overflow family also uses the existing, separately proven four-cell Boolean
reduction rewrite; this is retained as a distinct family rather than attributed
to density. Its proof and exact selected root are included.

The compiler is committed result `be4c5d3445e673577a018637068a48c25786bd9a`, tree
`f5a88c89733f769272c8ee515ea533d5ed8ed356`, built from the recorded parent and
source-file hashes before committing those exact bytes. Its executable SHA256 is
`43b3c94c5c44d82f229d1df15cf90a419603c2eaadca3922f91cd6947d6e653a`.
`final-proof.py` verifies the committed source hashes, frozen executable and
input hashes, commands, preparation receipts and final artifact receipts.
The binary and compiled objects are excluded from the compact archive; their
identity remains recorded. Complete input and output artifacts are included.

Four separate CPU-only placement diagnostics retain predicted timing and debug
coordinates. The three completed routes reproduce all 318 RAM locations from
their corresponding placement diagnostics. The 16 microcode ROMs still occupy
columns 38/41, with overflow0.75 also using column 49. None reaches column 26 or
14. All selected placement iterations continue to rank the 192 framebuffer RAMs
west of the 16 ROMs. The full graph/alias comparison before routing, 48 kept
address FFs, 192 address edges, all RAM coordinates and negative proof cases are
in `analysis.json`. The original `keep` attributes remain; ordinary placement
can move these WEAK cells. They are not fixed BEL constraints.

## Final routed timing

All margins below are picoseconds from the final native analogue endpoint export.
Reference rows are complete beta-0.5 routes with the same family and compiler.

| Family / beta | Whole-design setup failures / WNS | CPU same-clock WNS | 192 ROM addresses: failures / WNS | 90 ROM returns: failures / WNS | Pixel WNS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline / 0.5 reference | 115 / -1402 | -1402 | 28 / -583 | 0 / +652 | -544 |
| Baseline / 0.75 | Route failed | — | — | — | — |
| Baseline / 0.90 | 537 / -5249 | -5008 | 45 / -733 | 0 / +768 | -5249 |
| Overflow17 / 0.5 reference | 125 / -1487 | -1487 | 75 / -704 | 0 / +605 | -1333 |
| Overflow17 / 0.75 | 277 / -1249 | -1249 | 72 / -913 | 4 / -361 | +1863 |
| Overflow17 / 0.90 | 283 / -2084 | -2084 | 47 / -744 | 3 / -1169 | +2341 |

The baseline0.75 subprocess returned 125 after the native router failed the arc
from `initial_ax[4]` FFOUT to GOUT. The log reaches iteration 402 with five
overused wires and records 4,414,813 wires expanded for the failed arc. No final
RBF, routed graph or timing export exists for that run; its failure log and
receipt remain included. No external route termination was used.

The other three subprocesses returned 1 for setup failures. Their minimum
related hold margins are +813, +767 and +767 ps respectively, with zero related
hold failures. Every improvement and regression remains in each complete raw
endpoint report and `*-all-endpoint-deltas.json`; no endpoint intersection or
best-path-only comparison is used.

## Coverage and proof limits

All three comparisons preserve all 17,199 exported endpoint/clock tuples, every
setup window and setup/hold applicability flag. There are 17,194 timed setup
rows and related hold rows; five asynchronous clock-domain rows remain untimed.
Every nonphysical top-level field is identical, including clocks, IO, output,
registered, unregistered, reset and forwarded-clock contracts, unsupported
identities and physical obligations. The 105 unsupported rows remain open,
`complete` remains **false**, and the primitive-clock-margin inventory remains
empty. Matching covered endpoints is not complete timing signoff.

The two constant-output inventories match except for driver BEL locations.
All 1,888 static endpoint identities, sink definitions, capture clocks and
constant values match. Their physical constant chains differ at 8, 10 and 9
endpoints respectively because route-through buffers differ. The 105 detailed
unsupported records also change physical locations, saved pin maps and immediate
driver/route-through metadata. Full differences are retained in
`*-physical-record-differences.json`; these records are not silently normalized
into an equivalence claim. Fresh routing changes helper inventories. The endpoint
comparison alone does not establish graph equivalence and does not waive any
missing IO or clock coverage.

`check-route-graphs.py` and the independently developed `route-graph.py` then
prove each completed routed graph against its own corresponding placed graph.
They preserve every original cell, function, parameter, connection, net alias,
logical pin state, polarity, attribute and BEL. Every generated identity buffer
must read the original FF DATAIN signal, have a unique generated net/alias and
sole FF consumer, carry exact BUF parameters and PIN_SIG states, and occupy its
FF half. All generated helpers and aliases must be accounted for; no graph
intersection is used. Complete physical pin-map differences and LAB-state
schema/hashes remain recorded separately. Thirteen mutations of the actual
baseline0.90 capture test the graph checker's rejection paths. This logical
preservation proof does not establish emitted LUT-mask, RBF or electrical
equivalence.

The endpoint comparison rejects deliberate removal of an endpoint, changed
setup window, added row field, changed clock contract, missing unsupported
inventory and nonfinal timing model. Placement proofs separately reject alias,
FF clock, keep, saved pin-map and clock-setting corruption.

Global beta changes placement of logic as well as hard blocks, so the timing
changes cannot isolate RAM density. These measured outcomes do not justify
promoting a density setting. The distinct compiler experiment using native
clock windows in hard-block assignment requires its own validation.

## Charges and retained evidence

The run charges are subprocess elapsed times and include phase-lease queue waits:

| Run | Charged seconds |
| --- | ---: |
| Baseline0.75 | 1774.0020519532263 |
| Baseline0.90 | 2382.273934912868 |
| Overflow0.75 | 479.1719777500257 |
| Overflow0.90 | 2302.1244601821527 |
| These four runs | 6937.5724247982727 |

The supplied prior completed total, including both reciprocal RAM swaps, is
15783.49161636922436 seconds. The cumulative total through these four runs is
22721.06404116749706 seconds. Later experiments are not included. There is no
remaining campaign route cap.

`capture.tar.gz` retains the density recipes, all four preparation checkpoints
and logs, all four route receipts/logs, all three complete final RBF/routed/
endpoint/timing bundles, both complete beta-0.5 reference bundles, frozen source
inputs and compiler identity, the overflow proof, placement analysis and final
comparison scripts/results. It preserves the original sibling-directory layout
used by the analysis helpers. Original execution receipts retain absolute host
paths; offline inspection uses the archive paths. Re-executing a route requires
the frozen compiler and normal device leases and is not part of readback.

`capture-readback.json` records the archive hash and every member hash and size.
The packager reads every member back and compares it to the retained source
bytes. `package-capture.py` only packages and reads existing evidence; it never
invokes a compiler, a route or hardware. No commit or push is performed by the
capture installation.
