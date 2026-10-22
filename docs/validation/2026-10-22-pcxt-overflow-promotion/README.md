# PC/XT AX15 promotion: paired serialized-route qualification

The existing default-off two-LUT factor rewrite can promote AX15 directly into
the balanced overflow root. This diagnostic preserves the complete logical
function through final routing, but fails timing and remains unqualified.
No compiler source or RTL changed for this experiment.

The frozen compiler is `cbb1e1f170a729c052eeb0d5572c66214696f4e9`, tree
`3689cfafc484378a2b8ab75bbe1a4d75dc340a23`, binary SHA256
`6834b09bc3ccc63105d3027db0c0415a6dc080cc923645126c3a0fca4ec65fde`.
The same raw synthesis, QSF/SDC, seed 1, timing weight 2000 and criticality
exponent 5 first produce an overflow17-only placement. CPU/system and microcode
remain 100 MHz, pixel 74.25 MHz. Exact inputs, commands and source identity are
retained. Captured compiler source files are taken directly from that commit.

The native `--reassociate-producer CELL --reassociate-input B` operation runs
with `--no-pack --no-place --no-route` on the placed/unrouted checkpoint. The
AX15-containing leaf changes LUT6 mask 0x1 to LUT5 mask 0x1. The AND3 root
changes LUT3 mask 0x80 to LUT4 mask 0x80, implementing `A & B & C & ~D` with
AX15 on new logical input D. Both retain their BELs:
`MISTRAL_COMB.16.21.54` and `MISTRAL_COMB.16.21.18`. All native movability,
protection, private single-user, truth-table, net/pin, legality and context
checks pass. No guard is bypassed and no other cell moves.

The root's ALM mate,
`system.machine.cpu.cpu.u_eu_core.eu_register_dx_MISTRAL_FF_Q_4_DATAIN_MISTRAL_ALUT3_Q`,
has a changed physical pin map during preparation, with identical function,
connections and logical polarity. Every affected pin is included in the proof.
After routing, this mate's complete cell record is identical between the pair;
its temporary preparation map change is not omitted from the evidence.

## Same-execution control and final timing

First checkpoint reload removes 18 isolated unused `$iobuf_i` net records and
omits 19,060 empty cell-port records. The independent reload ledger proves that
each removed net has no cell connection, top port or surviving alias. All
serialized pin states/maps and every remaining module field match after exact
canonical-bit mapping. An unchanged reload control is therefore retained and
used for the paired route; no unmatched records are silently intersected away.
Its predicted timing report is byte-identical to the fresh placement report.

Control `reload-baseline/placed.json` SHA256 is
`efbb32aba3d9d8837d1acfd57cc1a444890bee1091223b8ea8bbb29af3196ff9`.
Candidate `promote-ax15/placed.json` SHA256 is
`bad4152a4f88ffe0ddce149720e2ee050305107b4aa84703d365bf686af93571`.
The two full GPU routes have identical commands except those verified inputs,
hold the phase lease, emit final analogue reports and RBFs, and return 1 for
setup failure.

| Final native setup WNS | Serialized control | AX15 promotion |
| --- | ---: | ---: |
| CPU/system clocks[0] | -1.143 ns | -1.114 ns |
| CPU-to-microcode address clocks[0] to clocks[2] | -0.564 ns | -0.472 ns |
| Microcode return clocks[2] to system | +0.881 ns | +0.619 ns |
| Capture clocks[1] to system | +3.210 ns | +3.210 ns |
| Pixel clk_pixel | -0.495 ns | -0.213 ns |
| Failing setup rows | 163 | 160 |
| Failing related hold rows | 0 | 0 |
| Unsupported boundaries | 105 | 105 |

System-only failures are 140/141, address failures 21/18 and pixel failures 2/1.
Per-clock hold WNS values are system 0.800/0.800 ns, capture 5.829/5.829 ns,
microcode 4.506/4.502 ns and pixel 0.752/0.752 ns. The original worst
AX15-to-ROM-address endpoint improves from -1.143 to -0.514 ns (+629 ps), with
its new worst launch on DX. The overall CPU worst path instead becomes
`eu_add_select0_MISTRAL_FF_Q` to `eu_register_r0_MISTRAL_FF_Q_7`, containing
7.559 ns routing and 3.072 ns logic. The targeted branch improvement therefore
produces only 29 ps improvement in overall system WNS.

Every one of the 17,199 endpoint identities/rows, including 17,194 setup-timed
rows and five unrelated crossing rows, retains its native clocks, setup window
and timing classification. Constant-output, static and unsupported-detail
records are fully identical, as are clock, IO/reset/registered/unregistered
contracts and both HPS physical obligations. Schema 7 remains `complete=false`.
The candidate improves/regresses 7122/6605 setup margins and 6270/6273 hold
margins. Unchanged identity and zero negative hold rows do not waive these
regressions or establish physical coverage.

The original fresh raw-synthesis overflow17 route is also retained as context.
Its RBF, timing report, endpoint snapshot and routed JSON each differ in bytes
from the serialized control. `route-analysis.json` records exact hashes and
comparison results. That older execution model is not substituted for the
paired serialized control and supplies no isolated gain claim for this rewrite.

## Complete Boolean and graph proof

`promote-ax15/proof.json` checks all 256 assignments of the selected pair's eight
external raw signals. `proof-whole.json` independently checks all 131,072
assignments of the entire seventeen-source reduction. Both before and after
match `~(|DX[15:0]) & ~AX[15]`, using the same four LUT identities. Source FF
identity is established from raw synthesis bus indices. All three internal leaf
outputs are private, with exactly one root consumer and no external observer.
Outside cell functions, canonical connections, complete net aliases, top ports,
logical pin states, metadata, settings and clocks remain accounted for.

`proof-routed.py` then verifies each final routed artifact against its own exact
prepared input and composes those checks with the freshly rerun pair/tree proofs.
Both final graphs have the same 12,335 cells and 15,466 named nets, including the
same 1,887 route-helper identities. Every helper on **each** side is separately
proved to be a scalar, noninverting `MISTRAL_BUF` feeding exactly one FF DATAIN,
on the corresponding physical half, with one private output net, no additional
aliases or observers, and the required two-row identity function. Only after
that complete ledger is verified is its output substituted for comparison.
No cell/net intersection or name-pattern deletion stands in for this proof.

Every one of the 10,448 original cell functions and all connected bits, complete
alias classes, external ports, placement/nonphysical metadata and 79,112 logical
pin states are checked per route. Routing changes 5401/5399 physical pin maps
relative to the control/candidate prepared inputs while preserving logical
states; full source and routed JSONs retain the maps and the checker records
counts and hashes. Final serialized settings match. Seven negative mutations
are rejected: helper inversion, changed helper primitive, additional private
alias, external private observer, wrong FF consumer, changed original LUT truth
bit and original input inversion. Checker/source hashes and all ledgers are
retained in `proof-routed.json`.

This establishes the logical function and full graph correspondence of these
final JSON artifacts. It does not decode electrical routing, physical LUT/ALM
configuration, serialized LAB state, emitted RBF masks or device behavior.
Native timing/endpoint qualification and exact-artifact hardware acceptance
remain separate gates. The result is not a production-retained candidate.

Only these two routes are charged here: baseline 234.26934448210523 seconds,
candidate 241.62728354521096 seconds, total **475.89662802731619 seconds**.
The prior subtotal through spreader experiments is 13422.19918582588360 seconds;
adding this pair alone gives 13898.09581385319979 seconds. This is **not the
campaign cumulative total**, because completed parallel operand-driver routes
are deliberately excluded. No route was run for proof or packaging. The user
removed the total route cap; native route bounds and shared GPU leases remain.

This is host diagnostic evidence. No kit was contacted, and no production core
lock, seal, module/shared contract or hardware acceptance changed. Further work
must address the remaining ALU/address/pixel timing, preserve complete coverage,
meet the same-recipe multi-seed gate, and qualify a fresh producer/exact artifact.

The archive contains raw inputs, all preparation/reload/route commands and
receipts, complete paired and original-context RBF/report/graph bytes, source
snapshots, all proof scripts/results, native timing analysis and complete negative
checker results. Executables are excluded. Scripts preserve their provenance
paths; rebuild the recorded compiler and substitute extracted paths for replay.
The manifest hashes every included member, including the inputs and prior
preparation manifests; every member is read back and verified.

Archive SHA256: `55e4aedf031aea36c169b3690cc5cc1b180c9577f9ef45dd2c33f4cf74c0a963`; all 65 members pass SHA256 readback. Compressed size: 19,593,936 bytes.
