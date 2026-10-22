# PC/XT private-pair no-trial route and unused-output audits

The fresh private-pair budget4 route tried no candidate. All four final artifacts—RBF, routed JSON, timing and endpoints—are byte identical to its same-binary default control. It retains 13 setup failures, worst −434 ps, and zero checked hold failures. No private-pair timing gain or regression was measured because eligibility rejected every discovered group before a trial.

The route uses frozen source243c829c07de92a610ddb9811cf69abff1987a0a and executable SHA256 `e94f638ef244ca43daa607b53a375e895357ab74746d065c8e4e3ea9a9bab803`. Its only command difference from the same-binary control is `--critical-private-pair-budget 4`. The new route is separate from the accepted CPU LUT6 experiment; it does not inherit that experiment's reduction to ten failures.

The complete native comparison retains all 17,199 endpoint/domain rows, all 17,194 checked setup/hold rows, every clock/Fmax summary, and all metadata categories. The entire graph has 12,397 cells and 15,546 alias records. Coverage remains `complete=false` with 105 unsupported identities. This host-only diagnostic supplies no coverage qualification, same-recipe seed qualification, hardware evidence or electrical acceptance.

## Why no trial ran

The log records 13 negative rows/path witnesses, five bounded producer witnesses, zero eligible groups and `tried=0/4 kept=0`. Five exact filter reasons remain in the log and `no-trial-proof.json`. The intended MDA pair reached the existing output-coverage check, which rejects an untimed or unused boundary. This is an eligibility exclusion, not a failed timing acceptance trial.

The complete MDA graph audit finds 64 FF captures across all seven external source nets and the consumer output, plus two connected unused arithmetic outputs. The original earlier private-pair host audit silently stopped at empty user lists: its capture inventory was correct, but its claim of no unsupported boundary was incomplete for the stricter native walker. That earlier audit is retained unchanged alongside this explicit correction.

The separate CGA audit examines an ALUT3 candidate on the completed CPU-improved route. Its four input/output roots reach 66 FF captures and six connected unused arithmetic outputs through input C. The new permutation pass was not run in that audit. Both audits preserve all aliases, drivers, users, public observers, source-to-boundary paths and original emitted timing rows. MDA's captured rows have two failures, worst −328 ps, minimum hold +942 ps; CGA's have three failures, worst −434 ps, minimum hold +1315 ps. These are saved live-route rows, not recomputed timing.

A native structural reader built from frozen972 headers/objects confirms all eight unused outputs are ARITH SO/CO, have exact driver backreferences, native combinational-output classification, zero users, zero route wires, and no public/global/clock observer. It only parses saved243 checkpoints and restores architecture metadata. It performs no timing analysis, routing or acceptance trial. Its source, 258 headers, 81 recorded link-object identities, commands, successful logs and original failed MDA probe build log are retained. Native executables and link objects are omitted with exact hashes. No native execution is claimed by offline replay.

`verify_frontiers.py` independently reconstructs every saved frontier from the complete graph, compares every capture and unused boundary, checks the native reader's complete visited-net and driver/user inventories, and verifies saved/native route-wire counts. It also binds the original probe sources, frozen model identities and recorded results. This does not establish eligibility for a future guard extension: the current guard remains unchanged in the captured route.

## Explicit capture dependency

This compact capture requires the separately retained CPU capture archive with SHA256:

`2b2240ddd347411dc22a695cb5db0e53544b873e75183acf299ebc230101c8f1`

Its exact size is 52,961,638 bytes, with 494 members and manifest SHA256 `b37d3f3fe7cfa10e9048e1810782b8cb80f3da5c0eb6b93980e537354c341707`. That dependency supplies the complete default/CPU-selected/prior13 routes, original raw inputs and frozen source/host evidence. It is explicitly hash-bound rather than silently fetched or resolved from original paths. This capture includes all new private4 route artifacts, CLI validation, accounting, both complete frontier audits, the prior audit inputs not present in the dependency, native structural sources/headers and deterministic offline checkers.

Verify the exact extracted member inventory, sizes and hashes against `manifest.json` before replay. The archive contains only regular relative paths; extract into an empty directory after rejecting absolute paths, `..`, links and duplicate members. From the extraction root, provide the CPU archive explicitly and run:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/hydrate.py /path/to/cpu-capture.tar.gz
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/prove_no_trial.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/compare_timing.py
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/verify_frontiers.py
```

Hydration verifies the CPU archive hash, complete member inventory and every member hash before safely installing it under `dependency/cpu`. Repeated hydration verifies the complete installed file set. `archive-mode.json` maps original absolute identities to exact retained bytes; no checker falls back to the live worktree. The three proof checks must return zero and reproduce their captured JSON bytes. No compiler, device model, GPU or network is used. Native build/replay scripts are retained for provenance only and are not invoked offline.

The new private4 route charged 492.9281931621954 seconds. Its matched default had already charged 715.8551598787308 seconds; the pair sum in the complete comparison must not be charged again. Root accounting records 35 completed subsequent runs totaling 37848.00241666333687 seconds. This capture binds that cumulative record and independently verifies the new route receipt, not the full campaign ledger.

The compact archive SHA256 is `4c82c3d307ad1148abc7dfffdd1d1c0efac58dcb93b419cfac3f9fb02cafba91` (7,061,135 bytes, 308 members); manifest SHA256 is `e65bf2caa985766367df5ba2e811084d87767506831a2c764d17d5f111794f86`. Independent root extraction verifies both complete archives and reproduces the three offline proof outputs byte for byte. The supplied root-readback script retains its original out-tree paths; the explicit commands above support standalone replay.
