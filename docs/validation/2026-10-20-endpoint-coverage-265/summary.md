# Native static endpoint coverage, nextpnr #265

This change explains and safely classifies 1,888 of the 1,908 previously
unsupported endpoints. Twenty remain unsupported, so the known unsafe swap
candidate is still rejected and the original baseline bundle is retained.
This is a coverage improvement and guarded no-op, not a routed QoR gain or
100 MHz closure.

## Source and mechanism

The assigned checkout is `/home/deano/harnesses/pcxt-compiler-repair/nextpnr`,
branch `feat/bounded-placement-swaps`, based on
`70dfc6f8afc6f33588d1186853cb3acf8ee7e9a0`. Implementation commits are
`64f0eaac017c9a6e653aeb1c436eea4854e2e96d` and
`6bf435684ee84dbf2782fc33af687e7ceed990fc`; later commits add tests and evidence.
History and prior frozen artifacts/binaries were preserved. Nothing was pushed,
published, merged, deployed or run on hardware. FES production pins, RTL and
constraints were not changed.

Schema 2 adds `static_endpoints` alongside native dynamic clock-pair rows and
unsupported endpoints. A structural proof follows only direct `MISTRAL_CONST`
zero/one sources and identity `MISTRAL_BUF` chains. It verifies source port
ownership/direction, defined constant parameters, buffer topology and pin state,
no cycles, and a registered sink with constrained capture clocks. The proof
records source chain/net identity, value, source and sink parameters/pin states,
and capture clock/edge. Both trials must have exactly identical static proofs.
No zero-delay rows, fabricated margins or setup gains are assigned to constants.
Dynamic setup/hold, unrelated bounds, clock checks and the 20 ps gain requirement
are unchanged. Old/incomplete inventories still fail closed.

## Classification and remaining blockers

The checkpoint connectivity and native timing code explain every missing input:

| Class | Before unsupported | After static | After unsupported |
| --- | ---: | ---: | ---: |
| M10K inputs directly driven by constants | 1,866 | 1,866 | 0 |
| FF data input directly driven by constant | 1 | 1 | 0 |
| FF data inputs through constant-fed route-throughs | 21 | 21 | 0 |
| SDRAM capture FFs through IO-fed route-throughs | 16 | 0 | 16 |
| FF asynchronous clear from PLL lock logic | 4 | 0 | 4 |
| Total | 1,908 | 1,888 | 20 |

Constants have no launch transition, so absence of registered arrival tags is
expected; they do not need invented timing paths. The 16 SDRAM inputs originate
at `MISTRAL_IO.O`, not constants. The frozen SDC supplies only the 50 MHz board
clock and has no input-delay constraints; the checkpoint has no
`timing/io_delays` setting. Native pad inputs therefore have no modeled external
launch requirements. Completing this class requires justified SDRAM interface
constraints and supported IO timing; reusing the capture clock as a launch clock
would invent a requirement.

The four `ACLR` paths originate at the two PLL `locked` outputs, through one
ALUT2 AND cone. `delay.cc` classifies PLL `locked` as `TMG_STARTPOINT`, while FF
`ACLR` is treated synchronously. The endpoint API intentionally requires valid
registered launch/capture domains and rejects these asynchronous source walks.
There is no justified registered launch relation or recovery/removal model here.
Matching names, routes or bitstream bytes cannot waive these requirements.

`classify.py` traces the actual checkpoint connectivity. Compressed before/after
classifications retain all endpoint identities and source trees. An early
working assumption that all 37 route-through inputs were constants was disproven
by the fresh native export; the final accounting above includes all 16 IO inputs.

## Paired full-route validation

Both fresh routes use the same immutable compiler at source `6bf43568`, the
original portable FES fixture (`0a2ad172142ffbb5d48062e0752d5f4a8739c2e7`),
seed 1, unchanged QSF/SDC, 100 MHz requirement, GPU router, weight 2000 and
exponent 5. They use the existing shared FES phase lease, owner `pcxt-agent`,
maximum two GPUs, no vendor/device pins and no timing-allow-fail. Each invocation
has a 600-second bound. Both exit normally with status 1 for failing setup.
The historical `c2c01bda` compiler is a different baseline and is not used for
this acceptance comparison. No checkpoint-reload timing is used.

| Clock | Baseline MHz | Rejected candidate MHz | Baseline setup ns | Candidate setup ns |
| --- | ---: | ---: | ---: | ---: |
| System (100 MHz) | 89.581650 | 91.726288 | -1.163 | -0.902 |
| Pixel (74.250069 MHz) | 83.745079 | 86.790489 | +1.527 | +1.946 |
| ROM (100 MHz) | 103.767563 | 103.850456 | +0.236 | +0.241 |
| Capture (100 MHz) | 242.248077 | 242.248077 | +2.936 | +2.936 |

Aggregate hold passes in both routes; per-pair hold nonregression still fails.

There are 17,147 dynamic endpoint/clock pairs in each fresh inventory. Their
values equal the corresponding prior full-route inventories, and the 1,888
static proofs match exactly across this pair. All formerly unsupported endpoints
are accounted for by the static and remaining unsupported sets; none disappeared.
The available dynamic rows still show 15 newly failing setup pairs, 7,135 setup
margin decreases and 6,181 hold margin decreases. These are diagnostic counts,
not an intersection used for acceptance. `check.py` also exercises each genuine
new failure as an isolated guard regression; all 15 reject independently of the
whole-design incomplete-coverage rejection.

All four baseline selected artifacts (RBF, routed checkpoint, native inventory,
and timing report) are retained byte for byte. The fresh baseline/candidate RBFs
and clock summaries reproduce their respective prior full-route results. The
updated inventories necessarily differ from schema 1, but no timing result or
bitstream changed. The retained selection is a no-op. A smaller swap or route
preservation proposal was not launched: the 20 unresolved paths prevent any
candidate from qualifying under the unchanged guard, independently of the known
dynamic regressions. No broad search or reroll was performed.

## Tests, artifacts and restart

- 49/49 `CriticalCohort.*` tests passed against the immutable implementation
  test binary. The extended static test subsequently passed again against the
  immutable follow-up test binary, adding IO-fed route-through rejection.
- 13/13 driver tests passed, including real native static proofs, changed
  value/chain/capture/sink invariants, missing and overlapping coverage, native
  setup/hold regressions, exception retention, exact baseline bytes and synthetic
  safe acceptance. This is not a full-design safe-candidate acceptance result.
- 15/15 existing CLI tests passed. The enhanced baseline model/export CLI case
  also passed, checking schema 2, final-analogue marking, and empty coverage
  remaining incomplete (two empty CPU routes).
- Both preceding portable evidence checks passed, including exact previous
  artifact hashes. The new portable checker passed with full artifact checks.
- One native-test compile error (unqualified unknown-bit enum) and one driver
  fixture collision were fixed in follow-up commits; failed logs are retained.
  No compiler was tested while being relinked.

Artifacts are under `/home/deano/harnesses/pcxt-endpoint-coverage`; full paired
routes and receipts are in `paired/`, with `paired/retained -> baseline`.
`manifest.json` records exact compiler/test hashes, source identities, input and
output hashes, commands, receipts, clock summaries and budget accounting.
The production compiler was built at `6bf43568`; follow-up source changes are
tests/docs only. Its `--version` agrees with `6bf43568`. The follow-up native test
binary uses the unchanged configure-time version and has its own hash and test
source identity; it is not the routed producer executable.

The paired routes consumed **337.574 seconds**
(baseline 154.507, candidate
183.067), including all lease waits.
The complete CLI suite is charged 150 seconds, and its enhanced export case
19 seconds. Prior charged work was 1,444.156 seconds. Total is
**1950.730 / 3,600 seconds**, leaving
**1649.270 seconds**. Each full-design invocation remained
below 600 seconds; all experiments are terminal. No extra proposal run was
launched and no budget was extended.

Recheck without routing:

```sh
python3 docs/validation/2026-10-20-endpoint-coverage-265/check.py
python3 docs/validation/2026-10-20-endpoint-coverage-265/check.py \
  --artifact-root /home/deano/harnesses/pcxt-endpoint-coverage/paired
```

The next technical work is to establish sound external SDRAM input timing and
PLL-reset timing semantics. Preserve fail-closed acceptance until those gaps
are resolved. Only then can a safer bounded proposal qualify. This task does
not authorize changes to fixture clocks or FES production pins to fill the gaps.
The 26 inherited legality failures in unrelated LAB (24,8), earlier reload/full-
route timing discrepancy and inherited LUT-pair CLI expectation failure remain
unresolved; none is claimed fixed or requalified here. Prior affected-LAB
checks passed, but this is not a whole-checkpoint legality pass.

Parent owns review and external delivery for nextpnr #265 and FES #840. FES
acceptance still requires same-recipe two-of-three seeds, a fresh producer with
correct BUILD_ID, exact-artifact timing and separate hardware authorization.
