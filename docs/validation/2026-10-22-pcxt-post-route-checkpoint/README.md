# PC/XT post-route checkpoint evidence, 22 October 2026

The corrected compiler preserves the route-only timing improvement and exports its actual final wire trees. The fresh corrected route has byte-identical RBF, native timing and complete endpoint reports to the retained old repair result. It still fails timing: 13 setup failures, whole-system −434 ps and CPU −173 ps at the existing 100 MHz requirement. There are zero reported hold failures and 105 unsupported boundaries (`complete=false`). No hardware ran and no timing, coverage, electrical or exact-artifact acceptance gate closes.

The source repair is `bd87c5304353e1df36e2a4567c19312023fa59e8`, binary `7a403212895868095a11d2f5804c3bcd77be6881c4979f9b52b6060a83d1ffb4`. The old baseline/repair/move compiler is a361eac060ebc25f9f70ef3bbc126814069d346b, binary e1a121d0de5e496afd96328ed435e77c0b78f913fcfd3768283c01b12313deac. Every route uses the same original raw synthesis, QSF, SDC, seed 1, HeAP weight 2000/exponent 5, hard-block windows, corrected spreading and proven overflow-17 reduction. The corrected route adds the same explicit route budget 32 as the old repair; only its compiler changes. Source bytes are matched to the committed revision, and every tool/input/command/artifact identity is retained.

## Distinct results

| Execution | Setup failures | Whole system | CPU | Result |
| --- | ---: | ---: | ---: | --- |
| Original reference | 14 | −434 ps | −173 ps | Already retained reference |
| Old route budget 32 | 13 | −434 ps | −173 ps | Three accepted net-tree trials; stale JSON export |
| Old move budget 16 | 14 | −434 ps | −173 ps | 32 shortlisted, zero attempted, zero kept; all four result artifacts identical to reference |
| Fresh fixed route budget 32 | 13 | −434 ps | −173 ps | Identical RBF/native reports to old route32, corrected final physical export |

Across the route-only improvement, all 17,199 endpoint/domain identities, windows and flags match the original, and every native contract/category is preserved. Exactly five setup rows improve; none regresses and every hold value is unchanged. One PIC row changes from −289 to +186 ps. Two failing MDA-derived RGBI rows improve to −327/−328 ps and remain failing; two positive RGBI rows also improve. ROM-address setup remains +124 ps, return +1257 ps, pixel +2005 ps and minimum reported hold +822 ps. The complete native ledgers retain every row, including all failing/unsupported records.

## Complete final export proof

`analysis/final-comparison.json` compares all modules, all 12,397 cells, all 15,546 named nets/aliases and all 19 top ports. Every function, parameter, port, cell attribute, alias identity, pin map and LAB snapshot matches the old logical graph. Apart from creator, differences are confined to ROUTING strings. There are 14 changed raw alias strings across 14 net identities; 11 aliases change only serialized order. Exactly three complete canonical wire→(pip,strength) maps change, matching the three accepted decisions. Raw changes are retained, not normalized away.

`analysis/native-tree-proof.json` interprets all three full changed trees with a standalone reader linked from frozen copies of the corrected compiler objects. It verifies the actual driver source, unique root, every pip source/destination and ownership, connectedness, acyclicity and every physical sink. The reader performs no configuration, timing analysis, placement or routing. Two actual missing-source/missing-sink mutations are rejected. The complete unchanged original-function/routing-helper proof is retained separately: 10,448 prepared cells plus 1,949 individually accounted transparent helpers, with three graph mutations rejected. No helper matching uses an intersection.

These are final native graph/route witnesses. They do not independently decode the RBF, establish electrical board margins or prove whole-board RTL equivalence. Complete endpoint coverage remains open.

## Why the old physical export was insufficient

The old ordinary-route path saved pin/LAB and ROUTING attributes before the post-route repairs. Later JSON writing emitted those cached attributes. Its RBF/native reports changed while its JSON stayed byte-identical to the original reference. `local-route-repair-20261022/serialization-audit.json` preserves exact frozen source locations and the limitation. The corrected default-off hook refreshes physical attributes after repairs/final configuration, and live move timing snapshots use current native provenance. Focused native/CLI tests, a strengthened native test, exact source verification, and default/explicit-zero CPU preparations are retained. Default/zero placed bytes match each other; relative to the original, only creator differs and predicted timing is byte-identical. Those preparation checks are not an additional default-off full-route qualification.

## Negative move reproduction is retained

The original move run made no change. Its log cannot distinguish geometric filtering from placement preflight because it omits per-stage shortlist details. An isolated saved-checkpoint reload was attempted only as a diagnostic. Its strict complete-native comparison rejects: 152 endpoint identities have different margins, including five originally failing display rows (three launched by CGA VRAM, two by MDA VRAM) that become positive, and unsupported metadata loses explicit disconnected ports. The explicit diagnostic continuation sees 26 eligible negative-slack cells and five legal destinations, versus original 32/zero. Every temporary transaction is rolled back exactly and no net is routed. This cannot identify the original live rejection cause; complete differences and failed strict attempt remain in `local-move-repair-20261022/native-probe/`.

## Accounting and replay

The three existing full-route charges are queue-inclusive and owned by the parent ledger. Do not charge these again when ingesting the archive.

| Execution | Seconds |
| --- | ---: |
| old route32 | 566.1809189263731 |
| old move16 | 211.3501574532129 |
| fixed route32 | 702.9260488403961 |
| Sum of these three existing runs | 1480.4571252199821 |

The captured original reference was already charged separately. CPU preparations, native reader/preflight checks and offline proof replay add no full-route/GPU charge. There was no extra seed, reload-bitstream generation, kit operation or hardware programming in this analysis.

`capture.tar.gz` contains the old route, old move, fresh corrected route, original full reference, raw inputs, exact preparation dependencies, source snapshots/diff/identity, native/CLI logs, all receipts and full proof outputs. All archive members are hashed and read back. Tool binaries and the frozen object directory are deliberately omitted; exact hashes and build commands/source identities are retained. Re-executing native helpers or routes requires providing/rebuilding those exact compiler dependencies. `offline-verify.py` replays all JSON/graph/endpoint checks and binds every complete saved-tree record to the actual ROUTING strings without invoking a compiler or GPU; native pip decoding itself is retained evidence, not rerun by that script.

The next step is continued timing repair using this corrected final-state evidence. The remaining setup failures, 105 unsupported boundaries, board obligations and hardware gates remain open.
