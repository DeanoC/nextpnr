# PC/XT hard-block window recipe: seeds 2 and 3

All three fixed-recipe seeds fail setup. This is host-only diagnostic evidence; no seed passes timing qualification or hardware acceptance.

| Seed | Setup failures | Worst setup | Minimum related hold | Microcode address failures / worst |
| --- | ---: | ---: | ---: | ---: |
| 1 (already captured) | 14 | −434 ps | +822 ps | 0 / +124 ps |
| 2 | 358 | −1747 ps | +719 ps | 95 / −1198 ps |
| 3 | 56 | −425 ps | +781 ps | 25 / −384 ps |

All runs use the exact frozen e1a121d0 compiler binary, overflow17 input and native-window plus corrected-spreader recipe. Only `--seed` differs. Source identity and frozen inputs are recorded in each original route receipt. The two new full routes used the leased GPU wrapper; CPU-only preparations were performed afterward with the identical binary and original command, replacing routing outputs with `--no-route`.

`prove.py` verifies each complete preparation-to-route graph, including every original alias, function, connection, polarity and cell placement. Every generated route-through is individually proved to be an identity buffer with one FF consumer. The native final reports preserve all 17,199 endpoint/clock tuples, all clock windows and setup/hold flags, and all global clock/IO/reset/physical contracts. Thirteen graph corruptions and six endpoint corruptions are rejected.

The two preparations reproduce all original routed-cell BELs exactly. Physical pin maps and LAB snapshots are recorded; this does not independently prove their physical LUT masks or RBF correctness. Static and unsupported native payloads are checked against their own exact final graphs before any logical comparison. Raw physical metadata differs and remains retained: 2 constant-output records in each seed, 10/13 static chains, and 81 unsupported-detail records. All 1,888 static identities and 105 unsupported reasons remain logically identical after verified helper substitution. `complete=false` and empty native `primitive_clocks` remain unchanged; no missing coverage is waived. There are no negative related-hold margins, but 8,279/9,221 positive hold margins decrease relative to seed 1.

The original root `analyze.py` and `analysis.json` are preserved. `proof.json` adds complete graph and native-payload verification. The archive includes original routes, fresh CPU preparations, exact source/input references, scripts, all hashes and a verified manifest. The binary is identified by hash and excluded from the archive. Historical absolute paths describe the retained task worktree; scripts run there without remapping.

The two new routes charge 880.31142435548827 seconds. With the prior campaign total of 24244.02042089961413 seconds, the subtotal including these two routes is 25124.33184525510240 seconds. Seed 1 is not charged twice. Separate ADD routes are excluded from this subtotal; CPU preparations charge no route time.
