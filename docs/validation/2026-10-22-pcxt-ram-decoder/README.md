# PC/XT native RAM and decoder locality diagnostic

This host-only experiment moves two byte-2 microcode ROMs together with their
sixteen private one-hot ALUT4 decoders. It uses compiler source
`692a1c372b2b55620ba1fd5e43e0c14ae8ffa4d9`, FES base `f09356de`, the original
frozen placed checkpoint and its seed-1 route recipe. CPU/system and microcode
remain 100 MHz; pixel remains 74.25 MHz. No hardware ran, and no RTL, production
recipe, lock, seal or shared contract changed.

ROM0 moves from M10K(41,43) to (14,39), and ROM1 from (38,43) to (14,40).
The sixteen decoders move to explicit whole-empty ALMs in columns 10–17.
All FF, kept-cell and carry placement stays fixed. The plan considers all
24 address edges, 64 RAM-to-decoder edges, 32 decoder-to-FF edges and four
direct RAM-to-FF edges; every incident net and its complete fanout is retained.
These destinations conflict with the separate fourteen-ROM diagnostic.
Combining them requires a new noncolliding plan and native validation.

## Native preparation and complete proof

A standalone helper links frozen compiler objects and does not alter the
compiler source or shared build. It verifies the exact two-RAM/sixteen-LUT
structure, complete one-hot masks, kept address/capture FFs, native free sites,
whole-ALM isolation and the conservative LAB input budget. It binds only the
selected cells and assigns pins only in the affected ALMs. Full touched-LAB
legality passes; no route-through insertion or global placement runs during
preparation. The first plan failed LAB capacity checks and remains captured.

Five injected failures and one rejected complete trial restore all serialized
logical state, cell architecture metadata, bindings/strengths, physical pin
maps and complete LAB state. Native physical LUT-mask computation agrees with
all 512 exhaustive decoder rows. The final exact-source checkpoint changes
only 18 BEL and 16 native-qualified pin-map attributes; every other original
byte is preserved and inverse replacement restores the exact source.

Normal placed-checkpoint restore rebuilds temporary COMB placement pin maps.
A separate read-only reload check therefore restores the saved physical maps
explicitly, checks they equal the serialized maps and proves all 512 rows
without assigning pins. Occupied, duplicate, wrong-type and protected-cell
plans all reject without producing a candidate.

After the full route, a separate native verifier restores serialized LAB state
without locking or parking cells and interprets the selected saved physical
maps. All 512 rows and full touched-LAB legality pass again. It does not route,
assign pins, decode the RBF or prove electrical/mux behavior.

Complete final graph comparison accounts for all 12,605 cells, 15,736 named
nets and 2,157 generated route-through buffers, without intersecting inventories
or discarding helpers. Every function, parameter, connection, alias class and
logical pin state matches. Only the declared 18 BELs and 16 physical decoder
maps differ; those final maps equal the native-qualified preparation maps.
Serialized LAB state is identical. Ten deliberate corruptions are rejected.
All 17,199 endpoint identities and clock/constraint tuples match, and every
non-endpoint coverage/contract record is unchanged.

## Routed result

The full route completes and writes the RBF, routed JSON and complete timing
and endpoint reports, then returns 1 because setup timing fails.

| Native setup group | Baseline failures / WNS | Candidate failures / WNS |
| --- | ---: | ---: |
| Whole design | 343 / -1.638 ns | 299 / -1.542 ns |
| System clocks[0], 10 ns window | 194 / -1.638 ns | 174 / -1.428 ns |
| All 192 CPU-to-ROM addresses, 2.5 ns window | 147 / -1.461 ns | 125 / -1.542 ns |
| Selected 24 addresses | 24 / -0.880 ns | 6 / -0.249 ns |
| All 90 microcode returns, 7.5 ns window | 1 / -0.358 ns | 0 / +0.775 ns |
| Selected 36 returns | 1 / -0.358 ns | 0 / +1.126 ns |
| Pixel domain | 1 / -0.228 ns | 0 / +1.084 ns |

Both reports have zero related-hold failures, with minimum related hold margin
0.795 ns. Nevertheless 6,124 setup margins and 7,854 positive hold margins
regress; the existing retention gates are unchanged. The six selected address
failures are bits 4, 6 and 11 on both moved RAMs. The worst overall address
path is unmoved byte-0 ROM0 bit 4 at -1.542 ns.

The result supports considering a RAM and its private decode logic together
when repairing placement. It does not close timing. The 105 unsupported
endpoints remain, `complete=false`, and native primitive-clock period/pulse
margin inventories are empty. Complete board/receiver/clock contracts,
same-recipe multi-seed qualification, a fresh producer and exact-artifact
hardware acceptance remain separate gates. The existing issues and draft PRs
are not closed by this evidence.

The route charge is **257.6850187298842 seconds**, for this run only. Cumulative
campaign accounting belongs to the parent handoff and is not recalculated here.
Shared GPU phase leases and native bounds were used; no GPU job was killed.

## Retained capture and reproduction

`capture.tar.gz` includes the exact candidate placed checkpoint, both native
normalization exports, complete final RBF/routed JSON/timing/endpoints, full
route log/receipt, constraints, helper/verifier/checker source, build commands,
all preparation/final proofs, malformed-input tests, complete small plans and
fanout inventories, and a SHA256 manifest. `readback.json` verifies every
archive member's size and SHA256 after reading it back.

The original placed source and complete same-binary baseline are already
retained in the sibling `2026-10-22-pcxt-reassociation/capture.tar.gz`, SHA256
`0511f300a4ece620dfb2323b3de7fc3d4f72e6787aae0da2b45b1fb4958f20cc`.
This source checkpoint has SHA256
`9241b02074cfee262c985c9ddde7e2c71f05bc654d35b989857a9a93ccc58878`;
the new candidate is
`dfaabdb773f788258dda3aa834899d840b7afad0837e8ad530bb371e7f84c95f`.

Large frozen compiler objects, model headers, source archive and executables
are excluded. Their hashes and source identity remain in the captured manifest
and build receipts. Rebuild the recorded committed compiler, recreate the
standalone helpers from captured source/build commands, and substitute extracted
paths for the original machine-local paths. The embedded compiler build label
is stale; use the recorded source/tree and binary hashes. The new route's
complete artifacts are included, not merely external hash references.

Archive SHA256: `89f47c9787f697f228ae0eff2503665983e9ccb5b85ddd451b12fca14787b19c`; 53 members pass complete SHA256 readback. Compressed size: 6,088,644 bytes.
