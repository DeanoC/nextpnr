# Post-route symmetric LUT6 swap: eligibility blocked

The paired fresh seed1 routes produced **identical RBF, routed JSON, final timing
and endpoint bytes**. The new option discovered all 13 negative native endpoint
witnesses but tried no swaps. Seven symmetric pairs, including the intended CPU
lane10 combine B/F pair, were rejected because the existing whole-LAB legality
check reported the original LAB illegal. This was an eligibility rejection,
before a timing trial. It provides no measured benefit from a LUT6 swap.

The other filters were 41 asymmetric pairs, one non-solitary LUT6 ALM and one
multi-sink input net. The full discovery, filter and decision lines are retained
in `decision-ledger.json`; preceding route32 decisions match between both runs.
Legality-model diagnosis is separate work. No legality check was relaxed here.

Both final captures retain 13 setup failures, worst margin −434 ps, and zero hold
failures. Seven CPU-source failures remain, worst −173 ps; three CGA-source
failures reach −434 ps; two MDA-source failures reach −328 ps. The remaining
−270 ps path launches from the firmware-loaded register into the PIC. All
17,199 native endpoint/domain rows match exactly, including the five untimed
rows; all 17,194 checked setup and hold margins are unchanged. All clock summaries,
clocks, windows, static/constant endpoints, interface/reset/primitive-clock
contracts, unsupported details and physical obligations are unchanged.

The complete graph check covers 12,397 cells, 15,546 net/alias identities, every
port and driver/user record, settings, BELs, masks, physical pin maps, LAB state,
and canonical wire/pip/strength trees. No cell or canonical tree changed. The
checker also verifies unique physical wire/pip ownership across the full design.
Its accepted-swap path is tested with an independent synthetic 64-row proof and
corruptions, but this capture contains no accepted swap to prove.

The default route matches the prior best13 RBF, endpoints and timing byte for
byte. Its entire routed graph differs only in the creator field. This is a
fresh-route comparison; reloading a checkpoint does not stand in for routing.

The frozen production source is commit
`461ce65469ce4e15e957c56800afb82a6da6ef96`, tree
`c2a9ab571a3eac7415d42677c7b8a11dd7172b5c`. The executable SHA256 is
`0fde11513b2468901c45ebb35c8755b4040527447e4c73ce73ceddbe0048cc6e`.
All 13 initial and final source files are captured. Native validation passed 33
cases, pure policy tests passed 3, and CLI checks passed 6. The initial fixture
failure is retained: it expected an empty second tree even though both original
roots were reserved. Only that test assertion changed; the failed case and the
19 skipped cases subsequently passed. Production bytes stayed fixed throughout
the CPU controls and paired routes.

Default and explicit-zero CPU preparations have identical checkpoint bytes and
match the prior compiler's complete graph and predicted timing, with only the
creator text changing. The initial preparation identity and receipts are
retained separately from the final test-only identity.

The current pair charged **1714.2566747143864 execution seconds**. Root's supplied
campaign accounting records 32 completed subsequent runs and
**35869.90418810164487 seconds** cumulatively. This package recomputes the current
pair charge; the previous campaign total is root-supplied accounting.

The compact capture contains raw synthesis/QSF/SDC inputs, complete route and CPU
preparation artifacts, original receipts, prior13 and prior-preparation
references, frozen sources, all validation logs, these checkers/results, and
accounting. The executable and build/object trees are deliberately omitted; the
binary remains bound by its original hash. Archive mode resolves recorded
absolute identities through an exact retained-path/hash map and never falls
back to the original workspace.

From the extracted archive, these read-only checks reproduce the captured proof
JSON byte for byte:

```sh
python3 final-analysis/verify_sources.py
python3 final-analysis/test_checks.py
python3 final-analysis/prove_graph.py
python3 final-analysis/compare_timing.py
python3 final-analysis/summarize.py
```

The finite checker uses complete set comparisons, including removed/added
identities. It records positive-margin regressions explicitly. Serialized
canonical route identities are not an independent electrical interpretation of
pip names; native transaction/RBF tests are distinct evidence. Final emitted
timing exposes the skewed inventory; the reference-free acceptance guard is
enforced in process but is not separately exported in this capture.

This remains a host-only diagnostic. The inventory is still `complete=false`
with 105 unsupported identities. Equality does not qualify that coverage, meet
100 MHz timing, satisfy the physical board obligations, or establish hardware
acceptance. No hardware ran.

Independent root readback verifies all 103 archive members and reruns all five
offline checks from a fresh extraction, reproducing every proof JSON byte for
byte. `root-readback.json` and the corresponding logs retain that result. The
archive SHA256 is `1b9642742873e2c6d4c14aba4d234fdddea2bd9e0b3f1b1ee030cbfd5230d4f6`.
