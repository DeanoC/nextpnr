# PC/XT routed LUT permutation and private-pair rejection

Compiler `186637013e17461a72cd1d2cd17ca5ef174ed06f` retains one fixed-BEL CGA ALUT5 D/E transposition. Its setup margin improves from −369ps to −234ps. All other 17,193 timed setup rows and all 17,194 related hold rows remain exact. The complete 17,199 endpoint identities, windows, flags, native metadata and clock summaries are preserved. There are still ten setup failures, worst system slack −434ps, and 105 unsupported entries with `complete=false`. This is host-route diagnostic evidence; timing closure and hardware acceptance remain open.

The capture contains three fresh routes using the same raw synthesis, QSF, SDC, seed1, 100MHz recipe and production binary SHA256 `7faa854df524d0dd3a69cf90622b589692d2db078c031a36ac2c7a5d0deadf12`: CPU LUT6 budget16 control; that control plus ordinary-LUT permutation budget4 and the explicit unused-arithmetic option; and the independent private-pair budget4 candidate with that option. The control appears once. No later compiler or active-route files are included, and no RTL, firmware, PLL, external timing constraint or shared contract changes are made.

The permutation proof compares every cell, alias, public port, LAB field and wire/PIP owner. All 32 raw input truth rows match for the accepted ALUT5. Exactly two canonical input trees change. Recorded native readback checks all six selected nets: 16 stationary users retain 46 original wire/PIP/strength tuples, and two physical sink locations change. The three rejected permutation trials remain in the decision ledger. Complete final reports verify every endpoint and every emitted timing category. The live pass used both timing frames; offline replay checks the emitted reports and never substitutes reload timing for that execution.

The private-pair candidate finds one eligible group but rejects its one attempted decomposition because routing `mda_font_address[11]` fails. RBF, timing and endpoints are byte-identical to the control. Routed JSON is not byte-identical: six route attributes differ only in serialization order. Complete canonical wire/PIP/strength mappings, cells, aliases, settings and all 17,194 setup/hold margins remain exact. The no-op topology proof records that no native tree re-execution was needed for this candidate.

All three complete RBF/routed/endpoints/timing/log bundles and receipts are retained, together with every proof/checker, independent review, all 131 frozen host-admission files and 29 exact compiler source blobs. Source blobs were checked against commit18663701, independently of newer working-tree changes. The native helper's final SHA256 is `689dbb4b864082394a8e9233a31293630220715f276c884e08c76aa28c0370c9`; its source, build receipts, initial failed compile, earlier reader32 identity, final controls and complete 401-member snapshot manifest are retained. Executables, snapshot payloads and four historical mutated-checkpoint payloads are omitted with exact identities. Portable replay checks their recorded outcomes; it does not rebuild or execute them. Native tree decoding is not an emitted-RBF mask proof or hardware test.

Recorded charge for the permutation pair is 1784.2650994360447 seconds. The independent private candidate adds 608.085521012079 seconds, without recharging the shared control. Immutable accounting context at FES commit `2439edf8dcbd1dc83dbb052640d29c5cfda67192` records 38 subsequent receipts and a cumulative 40240.35303711146057 seconds. Replay verifies that arithmetic and these three receipt hashes; other historical route payloads are outside this capture. Archive preparation and proof replay add zero route charge.

## Fresh extraction

The two explicit earlier archives below are required. Every member of both is verified before checking their retained excerpts. Complete dependency manifests, raw input bytes and the full prior CPU reference are included; original absolute paths are never used as a fallback. Transitive host-archive dependencies are provenance references, not fetched or rerun by this verifier.

| Dependency | SHA256 | Bytes | Members |
| --- | --- | ---: | ---: |
| [Unused-arithmetic host admission](../2026-10-22-pcxt-unobserved-arithmetic-host/README.md) | `bb96052b78d487b127e105a326be221c7c6e5e1b0082d68a2418b89af0c4cce1` | 28866191 | 173 |
| CPU source/route capture | `2b2240ddd347411dc22a695cb5db0e53544b873e75183acf299ebc230101c8f1` | 52961638 | 494 |

```sh
PYTHONDONTWRITEBYTECODE=1 python3 verify_capture.py \
  capture.tar.gz /tmp/pcxt-permutation-private-fresh \
  /path/to/unobserved-arithmetic-host.tar.gz \
  /path/to/cpu-source-route.tar.gz
```

The output directory must be absent or empty. The verifier rejects duplicate, linked, absolute and traversal archive members; verifies every member hash; and replays both complete graph, decision, truth, timing, native-topology and checker-corruption proofs. It then reruns the unchanged independent permutation review and requires all proof bytes and all extracted members to remain identical. No compiler, GPU, native helper, hardware or network operation is performed.

The archive SHA256 is `f5528953a89346e18cd10c1bc30afcf9e7d0b3f7da63d3628b04e8a7f5d6c855` (50,316,238 bytes, 261 regular members). Manifest SHA256 is `fd8aa884406761b2bc1c42ae282355a09cf929c92dcc6d85ab6dbe178cfb6196`. `offline-readback.json` records a successful fresh extraction, all 667 dependency members verified, both complete proof replays and exact independent-review reproduction. The independent-review receipt is `405b7197655af89f259db235e4582ec340a4428268e4535dcf84187b4fafee7e`.

The root agent independently repeated fresh extraction on the task disk; `root-readback.json` records the same complete passing replay. An initial extraction into the full shared `/tmp` filesystem stopped with ENOSPC before replay; `root-readback-enospc.stderr` retains that infrastructure failure. No route or compiler execution was added.
