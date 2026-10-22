# PC/XT whole-reduction balancing: seed-1 comparison

Compiler `cbb1e1f170a729c052eeb0d5572c66214696f4e9` extends the existing,
explicit `--balance-reduction-root` diagnostic from exactly sixteen literals in
four LUTs to thirteen through eighteen. Each selected private conjunction
becomes three product leaves and an AND3 root. Thirteen literals use 6+5+2;
fourteen through eighteen use 6+6+2 through 6+6+6. The existing sixteen-literal
6+6+4 result is unchanged. No default, placed-reduction scope or protection,
legality, endpoint or timing acceptance predicate changes.

This capture compares three fresh pack/place/routes from the same raw synthesis,
QSF/SDC, seed 1, timing weight 2000 and criticality exponent 5: no balancing,
overflow17 only, and both overflow17 plus last_result16. The selected functions
are `~(|DX[15:0]) & ~AX[15]` and `~(|eu_alu_last_result[15:0])`. Their exact cell
names, private intermediates and literal identities are in `whole-reductions.json`.
Both longest logic branches become two LUT levels without adding cells.

CPU/system and microcode retain 100 MHz; pixel retains 74.25 MHz. The authenticated
GPU phase-lease wrapper runs the frozen binary SHA256
`6834b09bc3ccc63105d3027db0c0415a6dc080cc923645126c3a0fca4ec65fde`.
`source-identity.json` binds the committed source/tree; the embedded CMake label
names the parent because the tested source was committed immediately after build.
The three runs complete routing and final analogue reports, write RBFs, and
return 1 because setup timing fails. No candidate closes timing or qualifies
for production retention.

| Final native setup WNS | Fresh baseline | Overflow17 | Both roots |
| --- | ---: | ---: | ---: |
| System clocks[0], including CPU and displays | -1.602 ns | -1.232 ns | -1.349 ns |
| CPU subtree, clocks[0] to clocks[0] | -1.602 ns | -1.232 ns | -1.126 ns |
| CGA subtree, clocks[0] to clocks[0] | -0.850 ns | -0.918 ns | -1.349 ns |
| MDA subtree, clocks[0] to clocks[0] | -0.339 ns | +0.546 ns | +0.460 ns |
| Capture clocks[1] to system | +2.854 ns | +3.210 ns | +3.070 ns |
| Microcode return clocks[2] to system | +0.001 ns | +0.397 ns | -0.046 ns |
| Pixel clk_pixel | +0.675 ns | +0.370 ns | -0.664 ns |
| CPU-to-ROM address clocks[0] to clocks[2] | -1.406 ns | -0.598 ns | -0.760 ns |
| Failing setup rows, all domains | 369 | 150 | 318 |
| Failing related hold rows | 0 | 0 | 0 |
| Unsupported boundaries | 105 | 105 | 105 |

The all-clock reported hold WNS values (baseline/overflow/both) are system
0.795/0.800/0.785 ns, capture 5.836/5.829/5.992 ns, microcode
5.124/4.511/4.859 ns and pixel 0.798/0.739/0.823 ns. These are positive reported
margins, not permission to reduce unrelated endpoint margins.

Every snapshot retains 17,199 endpoint identities and rows: 17,194 timed/related
rows and five unrelated crossings. Same-clock system setup failures are
221/130/248, CPU-to-ROM-address failures 148/20/67, microcode-return failures
0/0/1, and pixel failures 0/0/2. In the system domain, CPU failures are
166/101/214 and CGA failures 6/5/27. Thus the best CPU WNS in the both-root run
does not imply the fewest failing CPU endpoints or the best whole design.

All timed endpoint identities, setup windows, clock constraints and unsupported
identities match. Overflow-only improves 9009 setup margins and worsens 8183;
both improves 8843 and worsens 8335. Positive related hold margins decrease at
9071 and 8874 rows respectively. Full static/constant/unsupported metadata is
**not** identical after fresh placement: both constant-output records change,
5/6 static proof records change, and 81/80 unsupported detail records change.
Those complete records remain in the capture; comparison does not discard or
relax them. The snapshots stay schema 7, `complete=false`, with 1888 static
endpoints, no external IO contracts, and the two explicit HPS physical obligations.
The 105 unresolved boundaries and board/receiver/clock obligations remain gates.

The fresh cbb baseline's final RBF, timing report, endpoint snapshot and routed
JSON are each byte-identical to the prior fresh raw-synthesis run on compiler
`692a1c37`. They are each different from the older placed-checkpoint 692 baseline.
That older placement is not a same-recipe control for this experiment. Exact
hashes and small reference receipts are retained in `route-analysis.json` and
`reference-*-receipt.json`.

Eight focused native ReductionBalance tests and the CLI regression target pass.
The native cases exhaust supported bounds, mixed input/output polarity,
composition, identity, indexed consumer stores and unchanged rejection behavior.
The original sixteen-literal fixture exports identical complete modules between
692 and cbb with the feature disabled and with it enabled; all four JSON exports,
commands and comparison result are retained in `sixteen-*` and
`compare-sixteen.py`.

The independent unpacked proof compares an unmodified same-binary export with
each candidate. It retains all 31,392 cell identities, 34,471 canonical named
net identities and 19 top ports. Only four/eight selected cells change; every
outside function, connection, attribute, logical polarity, alias, net metadata,
module attribute and setting is preserved. Private outputs have one internal
consumer and no external observer. Both root truth tables are exhausted over
131,072 and 65,536 assignments for each comparison. The raw/first-factor subtree
audit and its separate exhaustive checks are also retained. These are complete
unpacked-graph proofs, not a final routed or physical RBF equivalence proof.

Final routed-proof feasibility was audited separately. All three designs retain
the same 10,448 non-buffer cell identities, with function/logical-pin-set changes
confined to the selected trees. Their route-through inventories differ:
2157/1887/2049 buffers, giving 12,605/12,335/12,497 total cells and
15,754/15,484/15,646 named nets. Relative to baseline, overflow removes 904 helper
names and adds 634; both removes 809 and adds 701. More than 10,300 original
non-buffer cells change placement, with 8142/7875 common physical pin mappings
changed. A complete final proof needs a per-artifact ledger for **every** helper,
its exact transparent function/polarity, private net, aliases, source ancestry
and actual consumers, plus verification of each selected routed root and all
outside connectivity. Intersecting inventories or dropping unmatched helpers
would not prove this. Existing same-placement pair proofs cannot be reused
unchanged. Physical LUT/ALM encoding and exact RBF/hardware acceptance remain
separate even after such a logical proof.

`route-*/analyzed-worst-paths.json` separates the actual CPU, CGA, MDA and system
paths. The earlier `route-both/worst-cpu-path.json` is preserved verbatim but is
misnamed: its endpoint is CGA `rgbi_MISTRAL_FF_Q_2`, the system worst path. The
actual worst CPU path in the both-root route is ALU select register to
`data_bus_word_out_MISTRAL_FF_Q_12`; it contains 7.653 ns routing and 2.891 ns
logic. The overflow-only worst CPU path remains AX[15] through the balanced
reduction and address-control chain into a microcode address register, with
7.736 ns routing and 2.948 ns logic. Neither path supports an isolated gain
claim for one rewritten LUT independent of placement.

The baseline/overflow/both runs charge
202.96359195606783/202.7812306820415/228.67172620724887 seconds. Their sum is
634.41654884535820 seconds. Added to the prior 9940.813688035588 seconds,
cumulative renewed route accounting is **10575.23023688094620 seconds** through
these three runs. The user removed the total route cap; native search bounds
and shared GPU leases remain. Later seed and placement-trace experiments are
outside this capture and this charge. No additional routes were run for this
analysis.

This is host diagnostic evidence only. No FPGA kit was contacted, and no RTL,
production recipe, lock, seal, shared contract or hardware acceptance changed.
The next integration work is review of the compiler/evidence, the unchanged
same-recipe multi-seed timing gate, complete endpoint/physical qualification,
a fresh producer and exact-artifact hardware acceptance. Draft status and open
integration gates are not closed by this capture.

The archive retains raw synthesis, constraints, complete route/RBF/report bytes,
commands, source identity, compatibility exports, proofs and analysis scripts.
Executables are excluded. Rebuild source cbb and substitute extracted artifact
paths for the recorded machine-local paths; helper scripts retain their original
provenance paths. `manifest.json` hashes every included member, including the
separate `inputs/manifest.json`; every archive member is read back and verified.
Archive identity and readback count are recorded alongside this README.

Archive SHA256: `e0dc43e835f8d6e12e479be28708e88221c262393bf6eeacdf672896c61d416d`; all 72 members pass SHA256 readback. Compressed size: 17,960,474 bytes.
