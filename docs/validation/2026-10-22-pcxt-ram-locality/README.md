# PC/XT microcode RAM locality diagnostics

These are placement experiments on compiler `692a1c37`, FES base `f09356de`.
They change only explicitly identified weak M10K BELs on an unrouted checkpoint.
All register/carry placements, keep attributes, RAM INIT, clocks and logical pin
states remain intact. CPU/system and microcode remain 100 MHz; pixel remains
74.25 MHz. Shared phase leases select the available HIP GPU. No hardware ran.

The first candidate changes six input bytes, moving lane-2 ROM3/ROM2 from
(41,34)/(38,34) to native free M10Ks at (14,36)/(14,37). Native checkpoint load
on compiler 66 verifies restored RAM pin maps before a complete route on 692.
The selected 24 address endpoints improve from nineteen failures to six, with
worst slack -1.461 ns to -0.107 ns. However the complete design regresses to
346 setup failures, and a moved ROM3-to-decoder-to-register return misses by
0.677 ns. Geometry alone is insufficient.

The second candidate changes exactly fourteen BEL strings (34 input bytes).
Its complete deterministic assignment uses eleven column-14 sites and three
column-41 sites, considering all 48 address FFs, 192 address edges, 154 immediate
output edges and 83 incident nets. It preserves the two measured destinations
and leaves lane-2 ROM0/ROM1 beside their east decoder bank. The selected 168
address endpoints improve from 123 failures to 80; whole-design failures fall
to 217. Detailed candidate selection and rejected geometric alternatives are
inside `fourteen-ram-capture.tar.gz`; none is described as a timing proof.

A third route combines the fourteen RAM moves with the independently proved
first-factor rewrite. Its Boolean baseline is the serialized first-factor
route from the reassociation capture. Exact graph proof verifies only the
fourteen additional BEL changes. It has 247 setup failures and also fails.

| Final native metric | Original placement | Two RAMs | Fourteen RAMs | Fourteen + factor |
| --- | ---: | ---: | ---: | ---: |
| CPU/system setup WNS, ns | -1.638 | -1.301 | -1.467 | -1.350 |
| Microcode return setup WNS, ns | -0.358 | -0.677 | -0.179 | -0.032 |
| Pixel setup WNS, ns | -0.228 | -0.306 | -0.681 | -0.282 |
| Setup failures | 343 | 346 | 217 | 247 |
| Related hold failures | 0 | 0 | 0 | 0 |
| Unsupported boundaries | 105 | 105 | 105 | 105 |

Complete final graph checks account for all 12,605 cells, 15,736 named nets,
83,426 pin states/maps and 2157 route-through identities in RAM-only runs.
Only declared BELs differ; serialized LAB state is identical. Eight mutation
checks reject. All 17,199 endpoint clock tuples match. Positive hold margins
still decrease (6491 rows for two moves, 7495 for fourteen), so these are not
accepted incremental repairs. Native primitive-clock margin inventories are
empty; ordinary data setup/hold rows cannot substitute for pulse/period checks.
The proofs cover primitive functions and graph identity, not decoded RBF muxes
or physical IO margins.

## Reproduction and retained evidence

The original input, constraints, compiler identity and baseline are in the
sibling `2026-10-22-pcxt-reassociation/capture.tar.gz`. Its chain capture supplies
the serialized first-factor baseline for the combined comparison. Each RAM
capture retains preparation code, exact commands, receipts, graph/endpoint
checks, final RBF/reports and source hashes. Executables are excluded. Captured
absolute paths must be adjusted to the extracted sibling bundles and a rebuilt
binary of the recorded identity; no production recipe is changed.

| Capture | SHA256 | Verified members |
| --- | --- | ---: |
| two-ram-capture.tar.gz | `3addd0e41119d0d6d3d9f11b0b28e518e966092f8dcda43525debfbbf7cd2346` | 27 |
| fourteen-ram-capture.tar.gz | `bb4ba6b0bbb9b5c5e0b3068282d0baa9c318c7dd38d86564a77eebc9bfc1025a` | 26 |
| combined-capture.tar.gz | `81048a56e19a556225b0fa906a0fc6b5505944535bd687859412d3221576c1bf` | 16 |

Every archived member passes SHA256 readback. Route charges are 183.897093,
235.426366 and 158.501834 seconds respectively. With the separately retained
fresh original-synthesis baseline (234.036640 seconds), cumulative renewed
accounting reaches 9940.813688 seconds. The user removed the total route cap;
all earlier charges and failed captures remain retained.

The fresh synthesis baseline on compiler 692 reproduces the original pipeline
candidate RBF, timing and endpoint bytes exactly (CPU -1.602 ns, microcode
+0.001 ns, pixel +0.675 ns, 369 failures). This differs from the saved placed
checkpoint baseline and is kept as a separate execution model.

The next justified locality experiment moves a ROM and its private decoder
LUTs together, with native pin assignment, full LAB legality and exact rollback.
Blind LUT BEL edits and blanket re-placement of packed JSON are unsafe: carry
clusters are absent from that serialization. Full placement should start from
the original synthesis input. These diagnostics do not change RTL, production
recipes, locks, seals or shared contracts. FES #840 and draft PRs nextpnr #266
and FES #979 remain open with the existing timing, coverage, multi-seed,
producer and exact-artifact physical qualification gates.
