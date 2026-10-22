# PC/XT bounded second LUT6 round, 22 October 2026

This capture retains two fresh seed-1 routes from compiler
`5d566b0d0b93cbcb3773e4a2bd6094bbccd80353`, binary
`e1f208fc6aa72910b639f503c0b5fec5140c15e613c715f3e5476ab7c4d706b7`.
The candidate adds only `--critical-lut6-swap-rounds 2`; both runs retain the
same shared 16-trial limit. All failed trials and preceding repair decisions
remain in the complete logs and decision ledger.

The second round retains a B/F swap on operand1 lane10 after the same first-round
B/A swap. Complete final graph comparison proves all 64 raw-input truth rows,
all aliases, cells, pin maps, native LAB state, ownership and stationary trees.
Exactly two canonical input route trees differ; five additional raw route
attributes differ only in serialization order. No independently captured
intermediate checkpoint is asserted.

All 17,199 endpoint rows and complete native clock/metadata inventories are
compared. Sixteen setup rows improve by 246 ps; the remaining 17,178 timed setup
rows and all 17,194 related hold rows are unchanged. CPU BP13 and CX14 captures
cross zero, reducing whole-design setup failures from 10 to 8. Worst slack
remains -434 ps; 105 unsupported entries and `complete=false` remain. This is
host route evidence, with no hardware acceptance or electrical signoff.

The retained native reader directly decodes all six target input nets and the
output net: seven complete trees, six stationary users, two changed physical
users and 30 retained original wire/PIP/strength tuples. Root independently
executed that same frozen reader and reproduced both decoded outputs exactly.
The reader was built from internally consistent `18663701` sources/objects.
Explicit compatibility evidence verifies unchanged decoding/model sources,
the unused rounds member and uncalled orchestration boundary. It interprets
serialized JSON; it does not execute new orchestration on old objects, assign
pins, configure an RBF, reroute or perform reload STA.

The archive contains both complete RBF/routed JSON/endpoint/timing/log bundles,
all 70 frozen host-admission files and 29 exact source files, source and commit
bindings, default/explicit-one controls, native records and independent reviews.
The original raw creator is retained: both prepared graphs are byte-identical
to the prior 7faa control. Source6/native33/CLI9 are the current host gates;
earlier suites retain their original provenance rather than a rerun claim.
Executables and the 401-member native source/object snapshot are omitted with
exact hashes and qualification manifests. This is an evidence replay archive,
not a standalone compiler rebuild bundle or an emitted-RBF mask proof.

The exact completed prior CPU16 control, raw input/QSF/SDC, prior source union
and native qualification are selected from the required sibling archive
`2026-10-22-pcxt-routed-lut-permutation/capture.tar.gz`, SHA-256
`f5528953a89346e18cd10c1bc30afcf9e7d0b3f7da63d3628b04e8a7f5d6c855`.
The verifier hashes every member of both archives and each selected dependency
binding. That archive's earlier transitive dependencies are retained as
provenance context; they are not fetched or rerun. No live original-path
fallback is allowed.
The unchanged corruption checker uses an older parser fixture directly. The
portable replay wrapper explicitly binds that fixture to the exact archived
e94f log and graph; its original checker and independent-review bytes remain
unchanged.

Recorded route charges are 714.176197880879 s and 690.2115861573257 s.
The frozen accounting context at FES
`785482cca21583ab4964056c8af2ce2ad3402d3b` has 40 subsequent receipts and
41,644.74082114966527 s cumulative charge. Replay checks journal arithmetic and
these two receipt hashes; unrelated historical route payloads are not reread.
Offline/native structural checks add no GPU route charge.

Run a fresh offline readback on a disk with room for the expanded artifacts:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 verify_capture.py \
  capture.tar.gz /path/to/empty-readback \
  ../2026-10-22-pcxt-routed-lut-permutation/capture.tar.gz
```

The verifier checks complete member hashes, replays the graph, truth-table,
timing, coverage, checker-mutation and native-tree proofs, then reruns the
recorded independent reviewer and requires byte-identical outputs. It does
not run the native reader, compiler, GPU router or hardware. Further timing
closure, complete coverage and the required seed/physical qualification gates
remain open.

Root repeated a separate fresh extraction on the task disk. `root-readback.json`
records the complete passing member, dependency and proof replay. This added no
compiler, native reader, GPU or hardware execution.
