# PC/XT HeAP spreader correction: frozen seed-1 diagnostic

Compiler `be4c5d3445e673577a018637068a48c25786bd9a` adds the default-off
`--placer-heap-correct-spread` option. It corrects two independently tested
spreader errors: a one/two-cell partition uses its own local ranks rather than
absolute indices in the parent vector, and a right-to-left pivot transfer
updates the bucket of the cell actually moved. The old path remains selected
when the option is absent. No timing constraints, logical RTL, protection,
legalization or acceptance predicates change.

The tested binary SHA256 is
`43b3c94c5c44d82f229d1df15cf90a419603c2eaadca3922f91cd6947d6e653a`.
`source-identity.json` records parent `36b9e372`, exact source-file hashes,
result commit and result tree `f5a88c89733f769272c8ee515ea533d5ed8ed356`.
The tested bytes were committed unchanged after building. Collection verifies
all six captured source files against the result commit, not merely the binary's
embedded parent label.

Three fresh seed-1 routes use the same raw synthesis, QSF/SDC, 100 MHz CPU/system
and microcode clocks, 74.25 MHz pixel clock, HeAP timing weight 2000 and
criticality exponent 5. They compare the unchanged default, corrected spreading,
and corrected spreading plus the exact overflow17 reduction. The latter is the
already proved four-LUT `~(|DX[15:0]) & ~AX[15]` balancing recipe; this experiment
does not introduce another logical rewrite. Every run completes final analogue
timing and writes an RBF, then returns 1 because setup timing fails.

| Final native setup WNS | Default | Corrected spreading | Corrected + overflow17 |
| --- | ---: | ---: | ---: |
| System clocks[0] | -1.602 ns | -1.402 ns | -1.487 ns |
| CPU-to-microcode address clocks[0] to clocks[2] | -1.406 ns | -0.583 ns | -0.704 ns |
| Microcode return clocks[2] to system | +0.001 ns | +0.652 ns | +0.605 ns |
| Capture clocks[1] to system | +2.854 ns | +3.233 ns | +2.870 ns |
| Pixel clk_pixel | +0.675 ns | -0.544 ns | -1.333 ns |
| Failing setup rows | 369 | 115 | 125 |
| Failing related hold rows | 0 | 0 | 0 |
| Unsupported boundaries | 105 | 105 | 105 |

System-only failure counts are 221/84/47, address-domain failures 148/28/75,
and pixel failures 0/3/3. The worst system paths in these reports end in the CPU
subtree. Positive per-clock reported hold WNS values are system
0.795/0.784/0.772 ns, capture 5.836/5.826/5.836 ns, microcode
5.124/4.031/4.692 ns and pixel 0.798/0.816/0.683 ns. Fewer failing setup rows
therefore do not mean a closed design: the pixel clock newly fails, CPU/address
failures remain, and many individual margins regress.

All snapshots retain exactly 17,199 endpoint identities/rows, including
17,194 setup-timed rows and five unrelated crossing rows, with the same native
clock constraints and setup windows. Against default, corrected spreading
improves/regresses 9742/7448 setup margins and 7532/9659 hold margins; adding
overflow17 improves/regresses 8569/8620 setup and 8777/8412 hold margins.
The full constant/static/unsupported identities also match, but their physical
records are not equal: both constant-output records, six static records and
81/80 unsupported-detail records change. Every complete record is retained;
no record is intersected away or treated as qualified because its identity
survives. Schema 7 remains `complete=false`, with both HPS I2C physical
obligations. External board/receiver/clock coverage still needs qualification.

## Default and explicit-zero behavior

The default final RBF, timing report and endpoint snapshot are each byte-identical
to the frozen cbb fresh-route parent. The default routed JSON differs only in its
top-level creator string: complete modules are equal and replacing that one
string reproduces the exact parent bytes. The parent routed JSON and receipt
are included so that this comparison can be checked without relying on a
summary. The parent receipt hashes identify the three identical artifacts.

Native preparation also compares absent/default, enabled, explicit zero and a
conflicting dedicated/`--set` option. Default placement has identical complete
modules to the earlier frozen placement reference, with only the creator string
changed. Explicit zero has the same cell inventory and every BEL placement but
is not byte-identical: a complete 13,597-alias bijection accounts for two changed
numeric bit IDs throughout the entire graph, after which the sole difference is
the saved `placerHeap/correctSpread=0` setting. The conflicting setting fails
with return 125 and emits no checkpoint. This is stronger and more precise than
claiming that raw JSON differed only by settings.

All three focused CTest targets pass: HeAP control sets, congestion and spreading.
The spreading suite has ten tests. Independent mutation checks pass all ten with
the corrected helper, fail four when the absolute-rank bug is restored, and fail
two when the wrong-bucket transfer is restored. Test sources, exact helper
variants, build logs (including the initial invocation and C++17 retry) and run
logs are retained. Executables are excluded.

## Mechanism and evidence limits

The enabled option moves 10,365 of 10,448 cells in native preparation. All 318
M10Ks remain, including the 16 microcode blocks. The selected prior iteration is
6; the corrected run selects iteration 2. Both spreading traces still send all
16 microcode blocks to column 41; refinement then uses columns 38/41, with
changed rows. Prior/corrected final logged refinement wirelength is
113700/108070, while the timing-cost figures are 1791/2151. These are placement
metrics, not routed timing acceptance.

The repaired errors are real by independent tests, but the combined experiment
does not isolate either error's causal contribution to routing. The full traces
show solve/spread/legalize stage positions, not every recursive region merge or
cut. The analysis identifies density/rank allocation as a leading explanation
for persistent eastward ROM displacement; it does not prove the exact recursive
sequence. Additional macro-clearance observations and a phase-aware hard-block
assignment proposal in `analysis.json` are explicitly follow-up work, not
implemented or verified behavior in this capture.

No independent complete final routed logical-equivalence proof has been
established for either enabled route. Fresh placement changes route-through
helpers and physical pin maps. Such a proof must account for every helper's
function, polarity, aliases and connectivity, not compare only common cell
names. The default byte/module comparisons and preparation checks have their
stated scope. Even a complete final logical proof would remain separate from
physical RBF encoding and exact-artifact hardware acceptance.

The three routes charge 271.19433319894597, 275.0728820618242 and
204.33750373125076 seconds, summing to **750.60471899202093 seconds**. Added to
prior cumulative 12671.59446683386267 seconds, the total through these routes is
**13422.19918582588360 seconds**. The user removed the total route cap; native
search bounds and GPU phase leases remain. Parallel operand-driver and AX15
promotion routes are excluded from this capture and accounting. Packaging ran
no additional route.

This is host diagnostic evidence. No kit was contacted and no production core
lock, seal, module/shared contract or hardware acceptance changed. Both enabled
recipes fail timing and remain diagnostic. Integration still needs review,
same-recipe multi-seed timing closure, complete endpoint/physical qualification,
a fresh producer and exact-artifact hardware acceptance.

`capture.tar.gz` retains exact source snapshots and patch, tests, raw synthesis
and constraints, complete three-route/RBF/report bytes, all relevant placed
exports, both full successful trace logs, commands, receipts, analyses and
reference artifacts. `collection.json` records every source path/hash and
read-only verification. The original analysis/replay scripts retain machine-local
paths; when replaying, rebuild the recorded source, substitute extracted input
paths and reconstruct the reference layout described by `collection.json`.
Executables are excluded. The manifest hashes every member, including the input
manifest, and every archive member is read back and verified. Archive identity
and readback count are recorded alongside this README.

Archive SHA256: `5a739dee2433076bdf50810380c289716c7728d8e9358b7b2866d7d1f37b9cdf`; all 81 members pass SHA256 readback. Compressed size: 35,384,949 bytes.
