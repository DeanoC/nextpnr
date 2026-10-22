# PC/XT CPU round 2 plus display permutations, 23 October 2026

These two fresh seed-1 routes use compiler
`02ffa3aa6a74b71debbb23da24e5332b541cf742`, binary
`ff2174157ae425d4b8c106bb22120be740230ac02b28e8a198010124f3b433e3`.
Both use the existing two-round CPU LUT6 repair. The candidate additionally
enables the ordinary-LUT permutation budget of 16 and the separately qualified
unobserved-arithmetic-leaf option. The preceding route and CPU repair decisions
match exactly. The fresh control reproduces the completed CPU round-2 result.

Candidate trial 3 retains the CGA D/E permutation and trial 6 retains the MDA
A/C permutation. All 32 CGA and eight MDA raw-input truth rows match. Complete
comparison covers all 12,397 cells, 15,546 aliases, public ports, physical pin
maps, native LAB metadata and global wire/PIP ownership. Only canonical input
route trees 54479, 54480, 57715 and 57717 change. Raw serialization-order changes
are separately retained; they are not treated as physical changes. Every
rejected trial remains in the logs.

All 17,199 endpoint rows and their complete clock-pair windows and emitted
metadata are compared. One CGA setup margin improves 135 ps and four MDA setup
margins improve 62 ps each. The other 17,189 timed setup rows and all 17,194
related hold rows are exact. Both runs still have eight setup failures, worst
−434 ps, 105 unsupported entries and `complete=false`. This combines the prior
CPU and display gains; it does not close timing or qualify hardware.

The unchanged frozen native reader independently decodes both complete saved
routing frontiers: ten nets, 18 stationary users, four changed physical users
and 65 retained original wire/PIP/strength tuples. Root separately executes the
same reader and reproduces both native output files exactly. The reader uses
its own internally consistent old Context and only reads JSON/native source,
sink and PIP identities. Compatibility through `02ffa3aa` is inherited from the
exact budget16 source and checker identities. No C++ ABI objects are mixed and
no readback allocates pins, configures a bitstream, routes or performs STA.

The indexed M10K port resolver follows the pinned native frontend rule. This
capture retains the combined checker's former scalar-only source, correction
origin and independent review. The original budget16 rejection and its full
eight-negative resolver tests remain in the required budget16 archive. The
same corrected core and resolver bytes are verified here. No production
guard or alias inference was relaxed.

Complete host/source evidence is retained once in these required siblings:

- `2026-10-22-pcxt-lut-permutation-budget16/capture.tar.gz`, SHA-256
  `ab43d849cef04528dad1353c33e4a6bf440feffe39500d3a0509085c065a0b9e`:
  all 85 host-admission bindings, all 29 committed source files, compiler
  controls, indexed-resolver correction and reader qualification.
- `2026-10-22-pcxt-lut6-rounds/capture.tar.gz`, SHA-256
  `86105f2089992e257032b32b10c818b14e59c74a72bca14ba871c5cad890f5a8`:
  exact prior CPU round-2 control.

The verifier checks every member hash of both dependencies and every selected
binding. It checks all 85 host and 29 source hashes against the dependency
manifest and the identical committed validation receipt. Those host commands
are not rerun. Historical transitive dependencies are recorded context and
are not fetched. Compiler/reader executables and the reader's 401-member
source/header/object snapshot are omitted with exact hashes. There is no
live-path fallback.

Charges are 839.973974572029 s and 904.7418550127186 s. Frozen accounting at FES
`c02fede0378e36d61073bd77c89bfbcced94c084` contains 44 subsequent receipts and
45,003.56689689168647 s cumulative charge. Replay checks the journal arithmetic
and these two receipt hashes. Other historical route payloads are not reread.

Use a fresh disk-backed extraction directory:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 verify_capture.py \
  capture.tar.gz /path/to/empty-readback \
  ../2026-10-22-pcxt-lut-permutation-budget16/capture.tar.gz \
  ../2026-10-22-pcxt-lut6-rounds/capture.tar.gz
```

The verifier replays the complete graph/truth/timing/native-tree proofs, all
40 graph/log corruption cases and the original independent review. Outputs
must match exactly and the extracted inventory must remain unchanged. This is
host route evidence only: no native execution during offline replay, physical
mask or RBF equivalence, hardware acceptance, electrical signoff or complete
timing-coverage claim.
