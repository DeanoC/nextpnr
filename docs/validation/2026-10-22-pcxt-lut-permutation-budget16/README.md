# PC/XT permutation budget 4→16, 22 October 2026

These two fresh seed-1 routes use compiler
`02ffa3aa6a74b71debbb23da24e5332b541cf742`, binary
`ff2174157ae425d4b8c106bb22120be740230ac02b28e8a198010124f3b433e3`.
Only the explicit ordinary-LUT permutation trial budget changes from four to
16. The same CPU one-round repair and all first-four permutation decisions
remain exact. Trial 3 retains the existing CGA gain; trial 6 additionally retains
an A/C permutation in the MDA ALUT3. All rejected trials remain in the logs.

Complete comparison proves all eight raw-input truth rows for the new MDA
permutation, every cell/port/alias, complete native LAB metadata and all route
ownership. Exactly canonical net bits 57715/57717 change route trees; 12
additional raw route attributes differ only in serialization order. The saved
graphs independently prove the final change; no intermediate checkpoint is
invented from matching logs.

All 17,199 endpoint rows, clock pairs and emitted metadata categories are
compared. Four MDA setup margins improve 62 ps, including Q5 −327→−265 ps and
Q7 −328→−266 ps. The other 17,190 timed setup rows and all 17,194 related hold rows
are unchanged. Both runs still have 10 setup failures, worst −434 ps, 105
unsupported entries and `complete=false`. This is host route evidence, with
no hardware acceptance or electrical signoff. The separate CPU second-round
result is only a source/control dependency here; this pair does not combine
that result with the MDA change.

The first scalar-only graph checker rejected indexed M10K output ports. Its
original source and rejection are retained. The narrow corrected resolver
follows the pinned native frontend cell-bit naming rule, preserves serialized
driver/user coordinates, requires a unique exact saved physical pin-map key,
and rejects invalid indices, aliases and missing maps. Actual width 2
B1DATA[0]/[1], a literal scalar-name control and eight negative cases pass.
Independent review proves the rest of each graph checker unchanged. This
correction changes evidence interpretation, with no production source or
timing-guard waiver.

The unchanged frozen native reader separately verifies those actual M10K
IdStrings and source wires on both saved graphs. Its complete selected frontier
has four nets, two stationary users, two changed physical users and 19 retained
wire/PIP/strength tuples. Root independently executes the same reader and
reproduces both output files exactly. Compatibility binds the old internally
consistent `18663701` reader to `02ffa3aa`: model/parser/decoding bytes remain
exact, the rounds member is unused, and the uncalled permutation orchestration
differs only in its two budget-limit strings. No new C++ Context is passed to
old objects. No reader operation allocates pins, configures an RBF, routes or
performs reload STA.

All 85 frozen host-admission files and 29 exact committed source files are
retained. Native 45 and CLI 6 are the current host suites. Earlier coverage,
LUT6/private and reset-cache suites retain their original provenance. The
native-link audit retains the small test object and explains why the unchanged
native transaction test binary excludes CLI orchestration; fresh routes supply
the enabled 16-trial evidence. Default and explicit-zero prepared bytes and
predicted reports match the exact parent control, preserving raw creator data.
Executables and the separate 401-member native-reader source/object snapshot
are omitted with exact hashes and qualification manifests.

Two completed sibling archives are required by the portable verifier:

- `2026-10-22-pcxt-routed-lut-permutation/capture.tar.gz`, SHA-256
  `f5528953a89346e18cd10c1bc30afcf9e7d0b3f7da63d3628b04e8a7f5d6c855`:
  complete prior permutation4 control, raw input/QSF/SDC and reader qualification.
- `2026-10-22-pcxt-lut6-rounds/capture.tar.gz`, SHA-256
  `86105f2089992e257032b32b10c818b14e59c74a72bca14ba871c5cad890f5a8`:
  exact parent source and default preparation.

All members of both dependencies and every selected binding are verified.
Historical transitive dependency manifests remain provenance context; those
older archives are not fetched or rerun. The original budget-parser fixture
uses a direct historical path; the replay wrapper binds it explicitly to the
exact archived log while leaving the original checker and review bytes intact.
There is no live-path fallback.

Recorded route charges are 768.3349105152301 s and 845.7753356420435 s. Frozen
accounting at FES `23f66faebd2836ab6798b23276e57429b10e2a8d` contains 42
subsequent receipts and 43,258.85106730693887 s cumulative charge. Replay checks
journal arithmetic and these two receipt hashes; unrelated route payloads are
not reread. Offline and native structural checks add no GPU route charge.

Use a fresh disk-backed extraction directory:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 verify_capture.py \
  capture.tar.gz /path/to/empty-readback \
  ../2026-10-22-pcxt-routed-lut-permutation/capture.tar.gz \
  ../2026-10-22-pcxt-lut6-rounds/capture.tar.gz
```

The verifier replays full graph/truth/timing/native-tree proofs, 40 graph/log
corruptions, 13 budget corruptions, indexed-port controls and the independent
review, requiring exact outputs and a completely unchanged extracted inventory.
It does not execute native code, the GPU router or hardware, and does not claim
emitted-RBF mask equivalence. Timing closure, complete coverage and subsequent
seed/physical qualification remain open.
