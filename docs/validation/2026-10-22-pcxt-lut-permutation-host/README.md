# Frozen ALUT2–5 permutation host baseline

The generic fixed-BEL ALUT2–5 permutation implementation passed its corrected host validation, but no board route was admitted. The intended CGA target is excluded by the unchanged downstream coverage guard: one stationary input reaches six connected unused arithmetic outputs. This capture preserves that baseline before any separate unused-output guard extension. It contains no routed timing result for the permutation pass and no hardware evidence.

The committed source is `a17e0ce3762b78a30cbfd41c8615be9a9b4fc727`, tree `f6ebda5659aea1c32c256179c5ec7014a08b8b67`. The frozen production executable SHA256 is `7107f9fd57ee7a096c7f1153a989c105ea4ccaa9525998eac1fdc2cd1a56195b`. Its build parent is243c829c; the build label is not treated as the resulting source identity. The root source binding and final-validation SHA256 `f57472babbd03cbaf141cc3f7aed9403bc8ecee8a6908b71e528682a1de74de6` bind all 93 host evidence files and 26 complete source files.

## Initial failure and corrected execution

The initial45 native run is retained exactly:37 passed and8 failed. Two test equations incorrectly applied folded `PIN_INV` a second time when converting raw net values into physical LUT addresses. The correct native physical address bit is `!raw`, independent of `PIN_INV`, because the emitted LUT mask already folds that inversion. Only those two test expressions/comments changed. The production files and production executable remained unchanged.

The corrected run passed the same45 named tests, followed by39 retained native regressions and both6-test CLI suites. The corrected native-test SHA256 is `6205579494f0552b1b626ec63c5e472a4f24a7ca170715a61236c8829d23e66a`; the initial native-test identity was `bf0e03e4c660302b48276eec893455ffb4b463095c7ed300a149e90fde6916a6`. Both source snapshots, logs, receipts, test-only patch and independent correction review are preserved. The failed run is not relabeled as passing. Older cache8/private77 checks retain their original972 provenance and were not rerun here.

The native cases exercise fixed-BEL logical permutations with full raw truth rows, physical RBF masks, shared ALM mates, stationary sink routes, indexed user slots/holes, FF selectors, protected or malformed inputs, source/capture coverage and rollback after rejection or exceptions. These are generic host transaction tests. Their success does not establish actual-board eligibility or timing improvement.

## Complete disabled-behavior controls

The CPU-only preparations use the prior243 raw input recipe and frozen7107 executable, with default versus explicit-zero LUT6/private-pair/permutation budgets. Both completed without routing. Their placed JSON bytes are identical to each other. Against the prior243 preparation, every serialized field and raw byte except the creator string is equal; predicted timing bytes are identical. The verifier compares the entire modules/aliases/ports/settings and full raw serialization, not an intersection.

The CGA structural audit in the explicit frontier dependency confirms all four input/output roots,66 FF captures and6 unused arithmetic leaves. It derives these identities from the completed CPU route's saved graph and binds the native972 structural readback separately. No permutation pass was run for that audit, no reloaded timing is used, and this capture does not change its exclusion. `enabled_lut_permutation_route_admission` remains false.

## Compact capture and explicit dependencies

This capture retains every listed93 host file, all26 frozen source files, root precommit/source binding, independent reviews and deterministic offline checkers. It requires two separately retained immutable archives:

| Dependency | SHA256 | Bytes | Members |
| --- | --- | ---: | ---: |
| MDA/frontier | `4c82c3d307ad1148abc7dfffdd1d1c0efac58dcb93b419cfac3f9fb02cafba91` | 7061135 | 308 |
| CPU source/route | `2b2240ddd347411dc22a695cb5db0e53544b873e75183acf299ebc230101c8f1` | 52961638 | 494 |

The CPU dependency supplies original raw inputs, prior source/host provenance and exact control preparation bytes. The frontier dependency supplies the complete excluded-target analysis and native structural receipts. Neither is fetched automatically or silently resolved through original absolute paths.

Check the outer archive's exact regular-file member inventory, sizes and SHA256 values against `manifest.json`, rejecting absolute paths, `..`, links and duplicate members before extracting into an empty directory. Then supply both archives explicitly:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/hydrate.py /path/to/MDA-capture.tar.gz /path/to/CPU-capture.tar.gz
PYTHONDONTWRITEBYTECODE=1 python3 final-analysis/verify_host.py
PYTHONDONTWRITEBYTECODE=1 python3 dependency/frontier/final-analysis/verify_frontiers.py
```

Hydration verifies each complete archive and every embedded manifest member before extraction. It installs MDA under `dependency/frontier` and CPU under that dependency's own `dependency/cpu` directory. All original evidence identities resolve only through exact retained-path mappings. Repeated hydration checks the installed dependency inventories, with the explicitly declared nested CPU dependency verified separately. Both proof checks must exit zero and reproduce their captured JSON bytes.

`verify_host.py` verifies every current source/evidence hash, the committed source binding, exact initial and corrected named-test inventories, both CLI outcomes, unchanged production identity, full default/zero controls and the explicit target exclusion. `verify_frontiers.py` reconstructs complete saved frontiers and compares recorded native metadata; neither checker executes a compiler, device model, test binary, GPU route or hardware operation. Native binaries and build object payloads are omitted, with exact production and initial/final test identities retained. This archive is a replay of evidence checks, not a native build kit.

The capture's deterministic archive builder verifies every outer member, safely extracts into a fresh temporary directory, hydrates both dependencies and requires identical proof bytes. The external `offline-readback.json` records the resulting archive identity and replay results. No route time was charged by this host validation. It confers no timing closure, external-IO qualification or exact-artifact hardware acceptance.

The outer archive SHA256 is `3a9da14c90439588794b1c301df1357a28c15718ea8c729723fb7130e6c8d130` (26,863,329 bytes, 105 members), manifest SHA256 `4d413aa1c00d2159cc47aec74c19c44015bd06b2d1d8ac3bb760884ca0b5f149`. Independent root extraction verified the outer archive and both complete dependencies, and reproduced the host/frontier proof bytes. Root script retains its original evidence-tree paths; the explicit commands above support standalone replay.
