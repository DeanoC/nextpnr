# Private-pair candidate 0: rejected full-route result

Retain the previous 13-failure recipe. Candidate 0 increases setup failures to
82 and worsens worst setup slack from −434 ps to −829 ps. It clears none of the
13 previously failing endpoints and creates 69 new failures. Its predicted
four-row 851 ps gain does not survive the complete route. This capture does not
claim timing closure, production qualification or hardware acceptance.

Both cases are fresh routes of the original raw synthesis input, using seed 1,
100 MHz, HeAP weight 2000/exponent 5, native hard-block windows, corrected
spreading, the independently proved overflow17 root, and critical-route budget
32. The only case-selection addition is the explicit private-pair candidate 0.
Both use production binary
`57afe29115d5a144243b8d63af2035c1f7e43f65664bc53747e5879fa02381c8`.
The eleven final source files match committed `40097b8d`; source verification,
exact commands, inputs and hashes are retained. No placed-checkpoint reload
stands in for either run.

The default control reproduces the previous fixed-serialization route's RBF,
timing report and endpoint ledger byte for byte. Every serialized graph field
also matches apart from the creator string. The previous result is therefore
still the comparison reference, without a default-on behavioral change.

## Complete final native timing

Both routes return 1 for failed timing. They retain exactly 17,199 native
endpoint/domain rows: 17,194 checked and 5 unchecked. Every identity, domain,
window and flag is equal. Every other ledger metadata category is equal,
including all 105 unsupported boundaries and their complete details. Coverage
remains incomplete. No report intersection or inferred constraint is used.

| Native cohort | Rows | Default failures / setup WNS (ps) | Selected failures / setup WNS (ps) |
| --- | ---: | ---: | ---: |
| System → system | 14,520 | 13 / −434 | 82 / −829 |
| CPU captures within system → system | 2,699 | 7 / −173 | 69 / −829 |
| System → ROM memory clock | 192 | 0 / 124 | 0 / 124 |
| ROM memory clock → system | 90 | 0 / 1257 | 0 / 1165 |
| SDRAM clock → system | 16 | 0 / 3113 | 0 / 3113 |
| Pixel → pixel | 2,376 | 0 / 2005 | 0 / 1201 |

The CPU row is a subset of the system row, not additional endpoint coverage.
All native clock summaries, Fmax records, bounded paths, detailed report records,
histograms and their differences are retained in `route-comparison.json` and
the original reports. Report path lists remain bounded; the native endpoint
ledger supplies the full margin inventory.

Across all checked rows, setup margins improve in 7,038 rows, regress in 6,835
and remain equal in 3,321. Of those regressions, 6,824 started positive. The
largest setup regression is 4,438 ps. Hold margins improve in 6,077 rows, regress
in 5,809 and remain equal in 5,308. Every hold regression started positive; the
largest is 4,662 ps. No hold fails. The unchanged global minimum of 822 ps and
unchanged per-cohort hold minima do not hide those positive-margin regressions:
every original and selected margin is preserved.

All 13 old negative rows and their full saved source paths are explicit:

| Previously failing family | Default → selected setup margins (ps) |
| --- | --- |
| CPU data-bus output | −25 → −272 |
| CPU flag D | −63 → −32 |
| CPU AX | −6 → −22 |
| CPU BP | −173 → −206 |
| CPU BX | −25 → −259 |
| CPU CX | −136 → −169 |
| CPU DX | −45 → −61 |
| CGA-sourced RGBI Q1, Q2, Q3 | −26 → −166; −434 → −574; −369 → −509 |
| MDA-sourced RGBI Q5, Q7 | −327 → −655; −328 → −656 |
| PIC capture, firmware-loaded FF launch | −270 → −180 |

The display family comes from the actual saved launch cell, not the RGBI
capture alias. For MDA Q5 the worst launch moves from `vram.3.0.0 B1DATA[1]`
through the modified pair to sibling `vram.5.0.0 B1DATA[0]` through a different
ALUT4/ALUT5 branch. That sibling's first route costs 3.112 ns. Thus improving
the selected pair alone does not close the complete MDA convergence tree.
The worst CPU path changes from DS-register to BP-register at −173 ps to
opcode-immediate-register to SI-register at −829 ps; its routing and logic
components are 6.898 ns and 3.391 ns. Full source/capture cell and pin identities
are in the report, without interpreting generated names as RTL semantics.

The common bounded local repair keeps 3 of 3 actual trials in the default case
and 2 of 2 in the selected case, each within budget 32. Every proposal, trial,
reason and final graph binding is retained in `repair-decisions.json`. Local
proposal scores are not substituted for complete final timing.

## Logical and serialized physical evidence

`prove-graphs.py` replays the frozen 128-row preparation proof in memory without
rewriting it. Each final graph is separately checked against its own preparation:
all 10,448 original cells, original BELs, functions, pins, complete alias classes,
top ports and non-pin attributes are preserved. Each route adds 1,949 helpers,
for 12,397 final cells. Every helper is independently proved as an actual
noninverting primitive with one private net and exactly its FF DATAIN observer
before substitution. Equal helper names are not assumed equivalent. Nine actual
final-graph mutations reject.

Every preparation-to-route physical pin-map delta is retained: 5,361 in the
default case and 5,357 in the selected case. The full cross-route raw comparison
records 16,427 field differences and 10,569 differing ROUTING alias strings;
8,209 are serialization-order-only. Complete canonical wire/pip/strength maps
are retained for the other differences. This is syntactic tree accounting,
not a native device-connectivity or RBF-programming proof. Complete LAB snapshots
and original final artifacts remain available. Native pin/mate/RBF fixture tests
are separately captured; they are not whole-board physical acceptance.

The first strict settings-equality check rejected the graphs and its failure is
preserved. The exact sole difference is now explicitly accounted:
`gpurouter/localInputEscape` is absent in default and serialized as `"0 "` in
selected. The selected run reaches a congestion plateau. Committed router source
then reads `setting<bool>(..., false)`; the accessor inserts `std::to_string(false)`,
the string `0`. `Property::to_string` adds the trailing space to distinguish this
string from a bit vector; `from_string` removes that discriminator. Both paths
leave local input escape disabled. The two settings dictionaries remain recorded
as unequal. No field is normalized away. Exact committed router, accessor,
property and JSON-writer sources are retained under `settings-source/`.

The original preparation runtime identity remains unchanged. The later backend
fixture correction connects synchronous M10K B1EN, rejects async mode and fixes
formatting. All 48 pure/native and 7 CLI tests pass under the separately retained
final validation identity, with unchanged production source/binary. Initial
failed fixture evidence is also retained. The independent preparation proof and
complete emitted preparation comparison are under `../analysis/`.

## Accounting and replay

Default charge is 920.9708376382478 s; selected charge is 947.4481055433862 s.
The paired charge is **1868.4189431816340 s**. These conservative wrapper wall
times include lease waiting. Root already recorded both receipts once, bringing
the campaign ledger to 30 subsequent receipts and 34155.64751338725847 s.
Reference routes and CPU analyses are not charged again. No further full route,
source edit, kit or hardware operation was performed by this analysis.

The capture includes all source/test/preparation/final-route artifacts and failed
evidence, root's upstream-main check and exact accounting, plus all external
input bytes and the complete prior fixed-route reference. The read-only upstream
check found a build-portability change; it was not merged into these frozen runs.
The large executable and test object trees are omitted; their identities and
source provenance are retained. Offline replay verifies archived bytes and
recomputes the complete graph, native report and repair-ledger analyses. It does
not rerun place/route or reconstruct the executable. Archive manifest, full
member readback and offline replay receipts are outside the archive.

Keep candidate 0 disabled. The next compiler/RTL experiment must address complete
converging paths and be judged against the retained default control under the
same full endpoint and hardware-qualification gates.

## Durable archive and independent replay

`capture.tar.gz` contains 121 members, including every source/input/report byte
bound by `manifest.json`. Its 91,304,284 bytes have SHA256
`18c8074b7fedba429cabec99d2a337a64b827506a12a0f7d2b06d1d43ae7b237`.
Paths in the analysis above refer to the extracted archive.

Root independently verified every member and replayed all three analyses.
The graph and repair ledger reproduce exact bytes. The report comparison
exposed Python-set key-order nondeterminism; its initial byte mismatch is
retained. The [replay supplement](replay-order-supplement/README.md) checks
all values, types, keys and array order, allows only the four explicitly
enumerated margin-key permutations, and retains each permutation. All six
checks pass on two independent fresh extractions, including three explicit
hash seeds. Their complete result receipts are equal. The original archive
and checker remain unchanged; every timing regression remains recorded.
