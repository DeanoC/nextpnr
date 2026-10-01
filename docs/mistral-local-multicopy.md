# Repeated local remapping of one control root

The opt-in local remapper can retain several equivalent copies of the same
outer LUT, each serving a separately selected FF-enable cohort. A fully routed
RAM-test profile reaches **117.771759 MHz memory**, up **0.428413 MHz** from its
accepted 117.343346 MHz reference. Capture Fmax and some positive margins
decrease; the 130 MHz memory target remains open.

## Generic naming behavior

The first copy preserves the legacy `<outer>$local_remap` cell and paired
`<outer>$local_remap$Q` net names, including their existing ID allocation order.
If either name is occupied in the cell, net or net-alias namespace, the
allocator chooses the first free paired suffix: `<outer>$local_remap$1` and
its `$Q` net, then `$2`, and so on. Rejected suffix probes do not intern IDs;
the finite search bound accounts for live namespace entries and fails closed
on size overflow. Existing owners are never overwritten.

This changes name allocation only. Report validation, Boolean composition,
protected-site legality, candidate budgets, transaction rollback and timing
guards remain unchanged. Each later stage operates on the remaining original
root consumers and must qualify independently. No RTL-specific names or
patterns are added to the compiler.

## Measured profile and result

RTL, synthesis JSON, QSF/SDC, BUILD_ID, seed 2 and GPU device 1 were fixed. The
original three local descriptors remained exact, with group budgets 8/1/1.
A fourth candidate 0/group 1 stage added one ENA consumer; a fifth added six.
All five stages used `optimize_pins: false` and fixed original FF placement.
The original two-consumer copy remained intact, and the original root's 109
consumers became 102. Other existing reduction, internal-cut, capture,
decomposition and driver-copy selections remained in the profile; the driver
candidate 0 freshly qualified against the current predicted guide.

The four-stage full route reached 116.522957 MHz and was rejected. The fifth
stage preserved its first four models and added the six-consumer copy. Its
full route completed in 761.636 seconds with the following reported clocks:

| Clock | Reference MHz | Five-stage MHz | Required MHz |
| --- | ---: | ---: | ---: |
| Memory | 117.343346 | 117.771759 | 130.005203 |
| Pixel | 93.492897 | 93.650490 | 74.250069 |
| Capture | 378.582397 | 259.491394 | 130.005203 |

Acceptance requires positive memory gain, passing non-target clock constraints,
positive headroom in every non-target clock-pair window, no final hold/min
violations, and independent native/physical proof. All 15,290 current native
paths and seven windows were checked with complete finite segment sums. All
six non-target windows passed, and the last signoff had zero hold/min warnings.
The separate all-clock Fmax nonregression diagnostic remains **false**.

| Reported headroom | Reference ns | Five-stage ns |
| --- | ---: | ---: |
| Capture to memory, 5 ns window | 3.283 | 2.495 |
| Memory half-cycle | 2.091 | 2.070 |
| Memory to pixel | 3.520 | 3.347 |
| Memory same-clock target | -0.830 | -0.799 |

Lower positive non-target headroom is explicitly accepted and disclosed.
The memory 130 MHz requirement is still missed by 0.799 ns; this is an
incremental target-clock gain, not timing closure or all-clock improvement.

The proof reconstructs every native driver/user slot, owner order, paired PLL
record, original full pin/BEL/metadata and complete LAB snapshot. Only the two
new LUT/private-net owners and seven named ENA transfers are admitted. Each
copy's actual pin states and four native inputs are evaluated on all 16 rows;
the source/driver-copy function is checked separately on eight rows, alongside
the arithmetic banks and immediate carry successor. Physical closure covers
18 LABs, 166 original FFs and
237 unchanged occupied banks. All 5,232 existing buffers have explicit named
owner/private-Q/unique-FF ledgers; four other private outputs and the driver
output have separate frames. There is no global raw-ID translation or buffer
collapse, and original physical pins and placement remain strict.

## Immutable provenance

Artifacts reside below the experiment's `out/ramtest-wide-reduction-264/`.
The source/archive/binary identity, controlled runtime, complete inputs and
outputs are hash-bound and rechecked. The principal SHA256/commit anchors are:

| Role | Identity |
| --- | --- |
| Measured source | `81243a1300304d7e598156c692be2fcbf2c16268` |
| Measured base | `5dd69a8ddece6280d576816d2277c73f80607f52` |
| Compiler ELF | `91a51253a44e74a10f6ac97fa1f3d7bdd402bc45fdede6f46ff054a5d519b5cf` |
| Source archive | `c9476dec61230fb01f03b1baafc5efae07288f069f88b3a27e97759bdee792ad` |
| Five-stage prefix proof | `a7e83fc3c117c614f289a0697e7ee30858d7812704e6324877b56105ab2bc3c9` |
| Full profile | `d32305c1cf99c1f3ead93fe11ba7e7539b3c88ae2b2b1b44b93346da3ddb5f64` |
| Full run | `5ed3c077a6beee28c651f6b8638969160abad05d1965f55fb4ae6b373abde8dd` |
| Accepted full proof | `6dcf8c4bb6b4b20811334ab4a87d52163756085428fdc3dbb67b84a29c795e65` |
| Generated RBF | `b2877830cbdff575af2f5e786168f3184f4de1308be14a80b17f98774a8b23eb` |
| Rejected four-stage proof | `a905b65ff5b925f8e6527be52a12469b8c65dadee2b6ad6cd7c9ed94ce6b66c9` |

The measured checkout passed 46 host test cases, including repeated-root
selection, listing rollback, namespace collisions, exhaustive truth and fixed
FF consumers. The publication checkout passed a fresh HIP build and 51 focused
backend/CLI cases, including the merged routed-report refresh regression.
The [machine-readable validation record](validation/ramtest-local-multicopy-2026-10-01.json)
records the invocation, actual clock-pair windows, artifact hashes and tests.
Publication
cherry-pick `08a1afdf` is based on merged main
`3f4acc0e3b471039a019f53b3674c9f0c1ca112d`; its allocator and backend-test bytes
match the measured files. Main also contains the later explicit non-RBF routed
report refresh, absent from the measured checkout. This measurement used the
unchanged RBF signoff branch; no fresh full RAM route on the publication
checkout is claimed.

This is host-side routed/model and exported physical-function evidence. It
does not independently calibrate or recompute STA, decode RBF/CRAM/pips,
prove hidden caches/GPU IDs, test hardware, or select a default recipe.
