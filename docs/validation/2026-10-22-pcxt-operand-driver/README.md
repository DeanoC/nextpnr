# PC/XT native operand-driver copies: candidates 0 and 2

Two existing, guarded LUT-driver copy candidates preserve the complete logical
graph through final routing, but neither improves CPU worst slack or closes
timing. Setup failures fall from 318 to 192/140 while CPU WNS worsens from
-1.126 ns to -1.285/-1.215 ns. They remain diagnostic; no compiler source, RTL,
clock constraint or acceptance predicate changed for this experiment.

The frozen compiler is `cbb1e1f170a729c052eeb0d5572c66214696f4e9`, tree
`3689cfafc484378a2b8ab75bbe1a4d75dc340a23`, binary SHA256
`6834b09bc3ccc63105d3027db0c0415a6dc080cc923645126c3a0fca4ec65fde`.
Both candidates and the reference use fresh raw synthesis, the same QSF/SDC,
seed 1, 100 MHz CPU/system and microcode, 74.25 MHz pixel, timing weight 2000,
criticality exponent 5, and both exact overflow17/last_result16 balancing flags.
The prior full fresh both-root route is the matching execution-model control;
no placed-checkpoint replay is substituted. Every input, report, source and
artifact hash and each exact command are retained.

## Native selection and preparation

The existing CLI requires fresh ordinary pack/place for driver copying; it
rejects `--no-pack`, `--no-place` and saved implementation steps. The initial
attempt to guide it with the final routed report fails before discovery with
`Stale remap report cell.` because that report contains route-added helpers.
No stale paths were dropped or rewritten. The matching fresh native predicted
report is instead accepted in full.

Its bounded search discovers five edges and performs 38 native probes, yielding
four candidates. Candidates 0/1 are alternative half-ALM sites for an ALUT3 copy;
2/3 are alternatives for an ALUT5 copy. The original 2.310 ns outer ALUT5-to-
arithmetic edge is absent from the placed critical paths and remains untested
by these candidates. Their exact distinct source/sink names are in
`analysis.json` and each preparation proof.

| Selected candidate | Copied primitive | Copy BEL | Native predicted gain | Affected endpoint/clock pairs |
| --- | --- | --- | ---: | ---: |
| 0 | MISTRAL_ALUT3 | MISTRAL_MCOMB.6.35.18 | 314 ps | 180 |
| 2 | MISTRAL_ALUT5 | MISTRAL_MCOMB.6.35.18 | 390 ps | 261 |

Both pass the existing source/net/protected-LAB/legality, complete affected
endpoint, native clock and hold predicates. Native timing covers the additional
input loads and the arithmetic carry-successor cache. The copied output
replaces exactly one arithmetic A input. The original source remains, including
its now-unused output, and no original cell moves. The gains above are placement
estimates, not measured final routed improvements. Neither selected edge appears
in the retained final critical paths; no isolated routed edge gain is inferred.

The full listing restores `placed.json` and its predicted timing report
byte-for-byte to baseline. Candidate preparations are separately reconstructed
from fresh raw input using their actual route command with `--no-route` and
route-output options removed. Those normalized recipes match their full-route
commands. Return codes are zero; they add no full-route charge.

## Complete function and graph proof

`prove-copy.py` verifies every original cell, net alias, top port, parameter,
attribute, physical/logical pin, placement, clock and setting. Only the exact
selected consumer connection changes. Each candidate adds exactly one named
LUT and one named private net, with one driver and one consumer and no external
observer or additional alias. All 10,448 original cells, including 179 carrying
explicit protection attributes, remain unchanged apart from that proven input
substitution. All 13,597 original named aliases survive. The copy has the exact
source inputs and logical pin states; exhaustive 8-row/32-row truth tables
verify ALUT3/ALUT5 equivalence, including the pin-state interpretation. Original
source, sibling users and every protected original remain accounted for.

`proof-routed.py` independently verifies each complete final graph against its
own exact fresh prepared graph, then composes those checks with the copy proof.
The reference has 12,497 cells and 15,646 named nets; each candidate has 12,498
cells and 15,647 named nets. All three contain 2,049 route helpers, with matching
identities, but the checker does not assume that equality. Every helper on every
side is proved separately as a noninverting scalar `MISTRAL_BUF` with the exact
FF DATAIN consumer, physical-half relationship, private output, aliases and
absence of other observers before it is substituted for graph comparison.
No graph intersection or pattern-based deletion substitutes for this ledger.

The final graph checks cover 10,448/10,449/10,449 original cells and
79,112/79,116/79,118 logical pin states respectively. Routing changes
5418/5419/5419 physical pin maps relative to each prepared input while preserving
logical states; the complete artifacts and change counts/hashes are retained.
All original functions, connected bits, aliases, top ports, placement and
nonphysical metadata match their prepared basis. Fourteen checker mutations are
rejected: for each candidate, copied truth, copied input polarity, private alias,
external observer, wrong selected consumer, protected original attribute and
routed-helper inversion.

This is a complete logical graph/function proof for the captured final JSONs.
It does not decode electrical routing, serialized LAB configuration, emitted
physical LUT/RBF masks or device behavior. Final native setup/hold/endpoint
coverage and exact-artifact hardware qualification remain independent gates.

## Final native timing and coverage

Each full route completes final analogue timing, writes an RBF and returns 1
because setup fails.

| Final setup WNS | Fresh both-root control | Candidate 0 | Candidate 2 |
| --- | ---: | ---: | ---: |
| Whole system clocks[0] | -1.349 ns | -1.349 ns | -1.352 ns |
| CPU subtree clocks[0] to clocks[0] | -1.126 ns | -1.285 ns | -1.215 ns |
| CPU-to-microcode address clocks[0] to clocks[2] | -0.760 ns | -0.703 ns | -0.703 ns |
| Microcode return clocks[2] to system | -0.046 ns | +0.480 ns | +0.477 ns |
| Capture clocks[1] to system | +3.070 ns | +3.070 ns | +3.070 ns |
| Pixel clk_pixel | -0.664 ns | -0.505 ns | -0.545 ns |
| Failing setup rows | 318 | 192 | 140 |
| Failing related hold rows | 0 | 0 | 0 |
| Unsupported boundaries | 105 | 105 | 105 |

System-only failures are 248/124/69, address failures 67/66/69, microcode-return
failures 1/0/0 and pixel failures 2/2/2. Per-clock hold WNS is unchanged at
system 0.785 ns, capture 5.992 ns, microcode 4.859 ns and pixel 0.823 ns.
Candidate 0's worst CPU path is opcode-immediate to add-overflow; candidate 2's
is prefetch-address to BIU segment prefix. Neither closes the original task.

All 17,199 endpoint identities/rows retain their native clocks, setup windows,
timing classifications and contracts, including 17,194 setup-timed rows and
five unrelated crossing rows. Complete constant-output, static and unsupported-
detail records are identical. Both HPS I2C physical obligations remain and
schema 7 stays `complete=false`. Relative to control, candidates 0/2 improve
6827/6839 setup margins while regressing 7122/6966; they improve 5868/5999 hold
margins while regressing 6365/6082. Positive reported hold WNS and lower failure
counts do not waive those regressions or establish physical coverage.

Only the two existing candidate routes are charged here: candidate 0
267.0616417163983 seconds and candidate 2 252.56041279202327 seconds, totaling
**519.62205450842157 seconds**. The reused baseline is not charged again.
AX15, spreader and other campaign charges are excluded. No additional full
route was run for reconstruction, proof or packaging. The user removed the
total route cap; native search bounds and shared GPU phase leases remain.

This is host diagnostic evidence. No kit was contacted and no production lock,
seal, module/shared contract or hardware acceptance changed. The source and
recipe remain reviewable, but neither candidate qualifies for production
retention. Remaining CPU/display/address timing, same-recipe seed gates,
complete endpoint/board qualification and exact-artifact acceptance remain open.

The archive retains the initial stale-report rejection and exact listing rollback,
fresh candidate preparations, source snapshots, proof scripts and negative
results, complete three-route RBF/graph/timing/endpoint sets, raw inputs and
constraint/guidance hashes. Executables are excluded. Scripts keep provenance
paths; rebuild the frozen compiler and substitute extracted paths for replay.
Every archive member, including the inputs manifest, is hashed and read back.

Archive SHA256: `1e59c67b8f71c41c547b6e852ba6b9f9bb03c28fb271a4831cbbb46eaddcf335`; all 68 members pass SHA256 readback. Compressed size: 22,700,618 bytes.
