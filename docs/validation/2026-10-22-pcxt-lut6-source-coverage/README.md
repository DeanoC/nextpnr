# PC/XT native source coverage and routed LUT6 repair

This is a paired fresh full-route host diagnostic. One symmetric lane10 OR6 B/A input swap reduced setup failures from 13 to 10. CPU failures fell from 7 (worst −173 ps) to 4 (worst −72 ps). Sixteen checked setup margins improved and the other 17,178 were unchanged. All 17,194 checked hold margins were identical. The system worst setup margin remains −434 ps and reported system Fmax remains 95.840522 MHz against 100 MHz. This result is useful local progress, not timing closure.

Both cases retain all 17,199 native endpoint identities, clock relationships, windows, flags and metadata. Coverage remains `complete=false` with 105 unsupported identities. Their unchanged inventory is not qualification of those paths. There is no hardware evidence, same-recipe seed qualification or electrical acceptance here. Final exported timing exposes the skewed native analysis; the reference-free acceptance guard is enforced in the frozen source and native tests, but is not independently emitted as a second final report.

## Frozen source and recipe

The source is commit `243c829c07de92a610ddb9811cf69abff1987a0a`, tree `60b7cc7bddeb4ab7989924aeb7a71a02036b0fd0`. The production executable SHA256 is `e94f638ef244ca43daa607b53a375e895357ab74746d065c8e4e3ea9a9bab803`; its build label records an earlier parent, so the label is not used as source identity. `committed-source-verification.json` binds all 21 frozen source files and the final host validation. The final validation SHA256 is `8d58d9bbae8c3c4026c2a3ca92c7021e1ef45dbac3b394bf91f2d8bcbb67663d`.

Both routes use the same raw synthesis inputs, seed1 recipe, tool executable and critical-route budget32. The candidate additionally enables routed LUT6 swap budget16. Private-pair options are absent. Full commands, input hashes, lease receipts, logs, timing, endpoints, routed checkpoints and RBFs are retained in `route-default/` and `route-swap16/`.

The fresh default reproduces the prior13 RBF, timing and endpoint files byte for byte. Its entire routed graph is identical except for the top-level creator string. Default and explicit-zero CPU preparations are byte identical; both retain the fixed972 preparation graph and predicted timing, with only the creator string differing in JSON across compiler versions.

## Admission and native tests

The older972 counterexample is retained under `counterexample/`, bound to its original source and object identities. It shows why downstream coverage alone was insufficient: a source LUT could combine a known clocked branch with a missing-clock, undriven or public-undriven branch while the known endpoint rows still looked complete. The new bounded source-origin walk rejects those origins and checks complete native clock/edge identities at each modeled input and output. It does not invent missing timing.

The current frozen executable passed 6 focused source-coverage tests, 33 LUT6 native tests and 6 CLI tests. Previous cache8, private-pair77 and private-pair CLI6 results belong to the prior972 build; their unchanged implementation/test bytes are bound by the retained source manifests, but those suites were not rerun on this executable. All 62 current host evidence files are retained. The initial topology-reader API compile failure and all reader negative controls are preserved separately; neither is presented as a successful check.

The actual-board eligibility probe and its independent replay cover both native skew frames, complete source cones, downstream captures and native location legality. The offline source checker independently traverses the complete raw checkpoint and matches the recorded origin sets: ALU 415 nets/251 FF origins, ALU_2 239/156, and lane10 combine 35/28 in each frame. This is structural/native admission evidence, not reloaded timing signoff.

## Exact retained change

Discovery found all 13 negative endpoint rows and path witnesses, 28 LUT6 arcs, 43 filtered pairs and 7 eligible pairs across 3 cells. It tried 3 and retained 1. The lane10 OR6 B/A trial passed; one ALU E/D trial exhausted the first-net local route budget, and ALU_2 D/E failed the minimum-gain guard. The decision ledger and complete logs preserve these distinctions.

The graph proof compares every one of 12,397 cells, 15,546 net alias records/classes and 19 public ports. Only the accepted cell's two input connections and canonical route trees for bits43272/43273 change. All LUT masks, cell types, BELs, pin maps, LAB state and remaining serialized graph/state are preserved; all 64 actual-net assignments prove the accepted transposition's equivalence. Whole-graph and report corruption tests are retained in `checker-tests.json`.

The native topology helper resolves every selected source, sink and PIP against the frozen243 device model. Both original roots and strengths are preserved. The two selected trees contain respectively 6/5 wires before and 10/3 after; the single sinks exchange A/B physical inputs. Every saved edge has native source/destination evidence, each tree is connected and acyclic, and each complete sink set is reached. Root's independent execution produces identical output bytes. The helper performs no route, timing analysis or RBF reconstruction.

`verify_topology.py` checks those complete saved trees offline against the recorded native PIP/source/sink output, and reconstructs the retained missing-source and missing-sink mutations. The recorded native reader rejects both, plus a wrong complete user identity. All mutated checkpoint payloads, original positive checkpoint, logs, selections and receipts are included. Offline replay verifies the evidence; it does not execute the omitted native helper or independently derive device-model identities.

## Capture and replay

`manifest.json` is the exact archive member inventory, with SHA256 and size for every retained payload. `archive-mode.json` maps every original absolute evidence/input path to its retained relative path; the offline checkers have no original-path fallback. All original route inputs, both final routes, prior13 reference, current host evidence, 21 frozen source files, older counterexample source references, topology helper source and its 310 frozen headers are retained. No private4 route or later ALUT2–5 experiment is included.

Production/helper/test executables and link objects/libraries are omitted. Their original exact hashes and sizes, where captured, remain in `archive-mode.json`, frozen manifests and build receipts; the topology snapshot records all 81 link objects/libraries. Native test identity and eligibility/counterexample link-object identities are also explicit. Python bytecode caches are omitted. These omissions prevent rebuilding or executing native checks from this archive alone; no native replay is claimed by the offline checks.

After verifying archive paths are regular relative files with no `..`, extract into an empty directory. Verify the exact extracted file set equals the keys of `manifest.json` plus `manifest.json`, then check every listed size and SHA256 before running the checkers. With Python3 and no compiler, GPU, network or original worktree access, run these from the extraction root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/verify_sources.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/test_checks.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/prove_graph.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/compare_timing.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/summarize.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/verify_topology.py
```

Each checker must exit zero and reproduce its captured JSON proof bytes. `archive.py` builds a deterministic gzip/tar, verifies every member, safely extracts into a fresh temporary directory and runs all six checks. The external archive/offline readback receipts record the archive hash, member count and exact proof-byte equality. They are not embedded recursively in the archive. Original native build/replay scripts and commands are retained for provenance, not invoked by this offline procedure.

The paired fresh routes charge 1485.1700353994966 seconds, independently recomputed from their receipts. The retained root accounting reports 34 completed subsequent runs totaling 37355.07422350114147 seconds; this capture binds that cumulative record but independently reconstructs only the current pair.

Independent root extraction verifies all 494 members and reproduces all six proof outputs byte for byte. The archive SHA256 is `2b2240ddd347411dc22a695cb5db0e53544b873e75183acf299ebc230101c8f1`; manifest SHA256 is `b37d3f3fe7cfa10e9048e1810782b8cb80f3da5c0eb6b93980e537354c341707`. Root readback receipts/logs are retained alongside the archive. The supplied root-readback script expects the archive under a sibling `capture/` directory, matching its original evidence layout.
