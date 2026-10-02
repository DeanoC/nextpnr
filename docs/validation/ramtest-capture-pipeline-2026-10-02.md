# Capture pipeline locality benchmark

Local record date: 2026-10-02. Machine-readable identities, measured options,
input hashes and all seven clock windows are in
[ramtest-capture-pipeline-2026-10-02.json](ramtest-capture-pipeline-2026-10-02.json).

The explicit budget-8, radius-24 capture pipeline pass followed by composed
LUT-pair-copy candidate 0 reached **107.30764770507812 MHz** memory Fmax:
**+1.4427337646484375 MHz** over its qualified budget-8 parent and
**+1.532318115234375 MHz** over the matched fresh main baseline of
105.77532958984375 MHz. Its independent exported native and physical comparison
passed, all six required non-target clock windows remained positive, and final
signoff reported no hold violations. This qualifies the combined measured
gain under the target-memory-clock rule.

The standalone budget-64, radius-24 capture pipeline trial independently
qualified at 106.1233139038086 MHz: +0.34798431396484375 MHz over fresh main.
The standalone budget-8, radius-24 trial independently qualified at
105.86491394042969 MHz: +0.0895843505859375 MHz. These earlier records remain
separate from the combined result.

| Clock | Fresh main MHz | Budget 8 MHz | Budget 64 MHz | Budget 8 + copy 0 MHz |
| --- | ---: | ---: | ---: | ---: |
| Memory | 105.77532958984375 | 105.86491394042969 | 106.1233139038086 | 107.30764770507812 |
| Pixel | 96.320556640625 | 95.56575012207031 | 93.90553283691406 | 94.89466857910156 |
| Capture | 375.0871276855469 | 375.0871276855469 | 375.0871276855469 | 375.0871276855469 |

All-clock nonregression is false: pixel Fmax decreased by 0.7548065185546875 MHz
at budget 8 and 2.4150238037109375 MHz at budget 64. The combined result loses
0.67108154296875 MHz pixel Fmax against budget 8 and 1.4258880615234375 MHz
against fresh main. Pixel and capture still
pass their required 74.25006866455078 and 130.0052032470703 MHz constraints.
Memory still misses its 130.0052032470703 MHz constraint in all three trials.
The combined complete memory path is 9.318999871611595 ns against a
7.691999912261963 ns window, leaving -1.6269999593496323 ns headroom.

| Clock pair | Window ns | Budget 8 complete delay ns | Budget 8 headroom ns | Budget 64 complete delay ns | Budget 64 headroom ns |
| --- | ---: | ---: | ---: | ---: | ---: |
| Async → memory rising | 13.468000411987305 | 1.7849999815225601 | 11.683000430464745 | 1.79100002348423 | 11.677000388503075 |
| Memory falling → memory rising | 3.8459999561309814 | 1.5470000067725778 | 2.2989999493584037 | 1.5470000067725778 | 2.2989999493584037 |
| Pixel rising → pixel rising | 13.468000411987305 | 10.464000254869461 | 3.0040001571178436 | 10.649000018835068 | 2.819000393152237 |
| Pixel rising → memory rising | 13.468000411987305 | 2.235000044107437 | 11.233000367879868 | 2.235000044107437 | 11.233000367879868 |
| Memory rising → pixel rising | 7.691999912261963 | 2.6650001257658005 | 5.026999786496162 | 2.648000046610832 | 5.043999865651131 |
| Memory rising → memory rising | 7.691999912261963 | 9.445999875664711 | -1.753999963402748 | 9.42300021648407 | -1.731000304222107 |
| Capture rising → memory falling | 5.0 | 1.733000063803047 | 3.266999936196953 | 1.733000063803047 | 3.266999936196953 |

The combined route uses the same seven windows and endpoint coverage:

| Clock pair | Window ns | Budget 8 + copy 0 complete delay ns | Headroom ns |
| --- | ---: | ---: | ---: |
| Async → memory rising | 13.468000411987305 | 1.7819999605417252 | 11.68600045144558 |
| Memory falling → memory rising | 3.8459999561309814 | 1.5470000067725778 | 2.2989999493584037 |
| Pixel rising → pixel rising | 13.468000411987305 | 10.538000136613846 | 2.930000275373459 |
| Pixel rising → memory rising | 13.468000411987305 | 2.235000044107437 | 11.233000367879868 |
| Memory rising → pixel rising | 7.691999912261963 | 2.690999910235405 | 5.001000002026558 |
| Memory rising → memory rising | 7.691999912261963 | 9.318999871611595 | -1.6269999593496323 |
| Capture rising → memory falling | 5.0 | 1.733000063803047 | 3.266999936196953 |

All three trials used device `5CSEBA6U23I7`, seed 2, ordinary HeAP with timing weight
10 and critical exponent 2, GPU router device 1, enable replication 4,
`--freq 74.25`, `--timing-allow-fail`, a detailed timing report and a 16,384-path
report cap. Each complete report contained 15,290 paths across seven clock
pairs. The only selected diagnostic environment was
`NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY`, using the fresh main pre-route
guide with `8 24` or `64 24`. No reduction, remap plan, LUT-pair copy or
driver-copy option accompanied the two standalone routes. Their full routes and
RBF signoff completed with exit code 0 in 862.6983340589795 seconds for budget 8
and 830.5435178589541 seconds for budget 64. The recorder checked production
and input byte stability before and after each run.

The combined route retains the same budget-8 pipeline request and adds
`--remap-lut-pair-critical` with the fresh post-pipeline pre-route guide,
`--remap-lut-pair-compose-copy`, and `--remap-lut-pair-candidate 0`. A finished
listing independently bound 56 qualified candidates and restored the complete
parent frame after every trial. Candidate 0 was then recorded in a separate
placed run and a full routed/RBF signoff run. The latter completed with exit
code 0 in 881.0144869019277 seconds. Exact prefix/listing/profile, compiler,
source and input identities passed checks before and after both selected runs.

The generic pass discovers a registered fixed-hard-block output feeding two
plain existing FFs on the same actual clock and edge, with the first FF's Q
used only by the second FF's DATAIN. It changes both FF placements without
changing the native graph, controls, initial values or two-register latency.
Budget 8 retained seven disjoint pairs and moved 14 FF BELs; budget 64 retained
37 disjoint pairs and moved 74 FF BELs. The comparison covers all 14,893 original
cells and exact original graph parameters, directions, metadata, ordered
ports, aliases and serialized consumer slots, including paired PLL ports.

Exported physical checks cover 16 whole LABs and 41,750 unchanged outside ALMs
for budget 8, and 43 whole LABs and 41,480 unchanged outside ALMs for budget 64.
Each route-through buffer has an explicit identity and packed-function ledger:
budget 8 has 4,957 before and after; budget 64 has 4,957 before and 4,971 after,
with the 14 new buffers checked separately. Nine unmoved FF control-selector
rows at budget 8 and 39 at budget 64 change numeric CLK/ENA indices. Complete
logical signatures and memberships prove a consistent injective renumbering
that retains their logical controls. The selector-to-routed-wire reservation
table is not exported or independently checked.

The combined proof preserves every original budget-8 FF BEL, including the
prior 14 pipeline moves. It adds exactly one `MISTRAL_ALUT4` named
`hps_ddr.port0.slot_free_MISTRAL_ALUT3_B$lut_pair_copy` at
`MISTRAL_COMB.19.28.0`, retaining the shared original inner and outer LUTs and
their other users. Only `hps_ddr.port0.skid_burstcount_MISTRAL_FF_Q_5.ENA`
changes from raw bit 105902 to 120708. Separate native and routed physical
checks exhaust all 16 four-input assignments and prove mask `0x4440`
equivalent to the original two-LUT function. Original graph parameters, INIT,
metadata, ordered ports, aliases, PLL tokens and consumer slots are preserved
apart from that declared clone and ENA edge; pipeline latency is unchanged.

The additional exported physical comparison covers all ten ALMs in each of
three whole LABs, all 14,893 original full pin maps and 41,880 exact outside
ALMs. Both parent and combined routes contain 4,957 explicitly ledgered
route-through buffers. Original occupied banks and shared-input functions are
exact. Only the independently proved ENA driver/raw bit is substituted when
comparing complete FF control signatures and memberships under an injective
CLK/ENA selector mapping. All other saved fields, including LAB ACLR-used
flags and ACLR indices, remain exact, as do unused-half CLK/ENA indices.

Each standalone final proof binds 119 artifacts and directly rehashes 2,328 source or
compiler files. Historical disabled compiler/source provenance also binds 932
retained archive members; no old archive is extracted or executed. The combined
proof binds 140 artifacts and rehashes 2,328 source or compiler files, covering
the complete parent ledgers and all additional prefix, listing, selected
placement, route, recorder and proof dependencies.
It executes no old operational main, authenticator, generator or aggregate
evaluator. Raw records
are local artifacts under
`/home/deano/kepler/worktrees/fes-fes-ramtest-compiler-gains-264-17a49c0d/out/ramtest-wide-reduction-264/capture-pipeline-264`.
They are not committed with this summary. The JSON records their filenames and
digests so the complete ledgers remain distinguishable from this publication.

| Measured identity | SHA-256 / commit |
| --- | --- |
| Compiler source | `a0432321e5ab99e20ec83e4fe97b652128b3c4b8` |
| Compiler ELF | `4db58a6de261b8542cce2c50f969f1f7b23b2c9c92150fa9a80697f88d185c27` |
| Source archive | `19c307f0fc22f5b766d8eba1f0bb7a282a52e0f5344a4674d2a76aa214fa7d29` |
| Fresh main baseline run | `3389f349e09a9462be09f1ff5c7d0196c8ec775bd0844235b0624642675b529c` |
| Fresh main pre-route guide | `d117e8952e4741f64ca14094df6c4db5a973f9de3a9dc038820852f7b5c0f512` |
| Budget 8 route run | `43ae1c42b28456f53fce8927276c00c58dee433dd0db2504b353b3183c8ea5c3` |
| Budget 8 proof | `b03e2b0770cdf46c287ceae90c8ba7fa6e2c9f98e62fc2a5f340730232ddd825` |
| Budget 64 route run | `3305e36039e6263c03d6bdd63670b523318678cddd558d2136e0f22dc310e38e` |
| Budget 64 proof | `05d91f7b946d4f3e00dc8be745fb67c3131d92e96451c9a9b98ab11892596b43` |
| Budget 8 + copy 0 placed run | `1ac34d488937d68b8a63d8b46b45433086cd42e36af6caf038b505ac08e09f9c` |
| Budget 8 + copy 0 route run | `a2ddc6830f6d75e11cb1e0fbd13a2d3b284db69554fc72505c27b531484c893d` |
| Budget 8 + copy 0 route profile | `218946fb41717c027fe7b54ce819a6dc1076b7aeff187dc22cf8e710c3e14dd7` |
| Budget 8 + copy 0 proof source | `e9d9833b390a433c0e87f0f667a10fd407985d441abad14e31dc50111ac81b16` |
| Budget 8 + copy 0 proof | `6241696ea763e4cc8c8961ea6e57800beeda05fe545779f833a8a04d443a3cf8` |

Final host checks on source `a0432321e5ab99e20ec83e4fe97b652128b3c4b8` passed
**32 native cases and 11 CLI methods**: eight capture-pipeline cases, 12
LUT-pair placement cases, ten LUT-driver copy cases, two LAB pin-map cases,
and 11 capture-pipeline CLI methods. The eight capture cases passed in 29.660
seconds; the 24 native regressions passed in 96.912 seconds; the final CLI rerun
passed in 202.29 seconds. The test ELF is separately identified in the JSON.

Publication production bytes are inherited from the measured source on top of
`feat/mistral-lut-pair-copies-264` ([PR #109](https://github.com/DeanoC/nextpnr/pull/109),
base `266e3ef7a0821c38a18a3076f7d8b9b1638a7cd2`). The records qualify the measured
ELF and explicit benchmark options; they do not qualify a newly built
publication binary or the current default recipe. No FES RTL, pins,
constraints or hardware configuration changed. The pass remains default off.

The separate frozen composed stack remains at **117.86892700195312 MHz** on
an older compiler, configuration and HeAP baseline. It is unchanged; these
fresh-main comparisons neither inherit its qualification nor compare against
it as a regression. The combined result uses the same measured compiler as
the standalone pipeline trials.

No 130 MHz memory closure, independent numerical STA, live pointer/cache/GPU
or full runtime identity, complete placement/routing/device legality proof,
independent route-mux/CRAM/RBF decode, default-recipe qualification or hardware
acceptance is claimed. Timing acceptance checks every emitted finite complete
path sum and the final reported holds, without independently recomputing STA
or extrema for unreported clock pairs. Selected runs rely on the compiler's
legality checks; the independent physical comparison validates exported
snapshots and function scope. RBF hashes identify output bytes only.
