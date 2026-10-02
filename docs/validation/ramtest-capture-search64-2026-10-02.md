# Capture pipeline search after legality checks

Local record date: 2026-10-02. The explicit budget-64, radius-24 capture pipeline
trial reached **107.5037612915039 MHz** memory Fmax: **+1.7284317016601562 MHz**
over matched fresh main at 105.77532958984375 MHz and
**+1.3804473876953125 MHz** over the earlier 16-geometry budget-64 result.
The independent exported graph and physical comparison passed. All six required
non-target clock windows stayed positive and final signoff reported no hold
violations. Memory still misses its 130.0052032470703 MHz constraint.

The generic search now examines up to 64 ranked distinct LAB pairs per chain,
while retaining a limit of 16 timing trials. Illegal geometries and failed
snapshot-preservation checks restore the pair without spending a timing trial.
This lets the search reach useful placements after an illegal prefix. The
worst case permits four times as many placement snapshots and geometry checks
as the earlier 16-pair search; it does not raise the timing-trial cap or relax
the timing, legality or rollback guards. Discovery still uses actual primitive
types, registered clock arcs and graph structure, without matching RTL names,
pins or saved coordinates. It moves two existing plain FFs without changing
their graph, controls, INIT or two-register latency. The option stays default off.

The measured placement retained 53 disjoint pairs, moving 106 FF BELs. Eighteen
retained chains first reached timing analysis after more than 16 legality
rejections. The previously limiting read-capture chain first reached timing
analysis at geometry 34, after 33 legality rejections, and was retained.
Each chain reports attempts, legality/preservation rejections, timing trials
and the first timed ordinal; at most four rejected geometries are logged in
detail. A new native fixture exercises that illegal prefix and exact rollback.

| Clock | Fresh main MHz | Search64 MHz | Change MHz | Required MHz |
| --- | ---: | ---: | ---: | ---: |
| Memory | 105.77532958984375 | 107.5037612915039 | +1.7284317016601562 | 130.0052032470703 |
| Pixel | 96.320556640625 | 94.98480224609375 | -1.33575439453125 | 74.25006866455078 |
| Capture | 375.0871276855469 | 375.0871276855469 | 0 | 130.0052032470703 |

Pixel Fmax decreased, so all-clock nonregression is false. The result passes the
agreed target-memory-clock rule: memory improves, pixel and capture meet their
required clocks, all six non-target windows remain positive, and final reported
holds pass. Pixel full-cycle headroom falls by 0.14600002765655518 ns to
2.9400003850460052 ns. This positive margin reduction is disclosed rather than
treated as an unchanged result.

| Clock pair | Window ns | Complete delay ns | Headroom ns |
| --- | ---: | ---: | ---: |
| Async → memory rising | 13.468000411987305 | 1.794000044465065 | 11.67400036752224 |
| Memory falling → memory rising | 3.8459999561309814 | 1.5470000067725778 | 2.2989999493584037 |
| Pixel rising → pixel rising | 13.468000411987305 | 10.5280000269413 | 2.9400003850460052 |
| Pixel rising → memory rising | 13.468000411987305 | 2.235000044107437 | 11.233000367879868 |
| Memory rising → pixel rising | 7.691999912261963 | 2.5590000599622726 | 5.13299985229969 |
| Memory rising → memory rising | 7.691999912261963 | 9.302000254392624 | -1.610000342130661 |
| Capture rising → memory falling | 5.0 | 1.733000063803047 | 3.266999936196953 |

The route used device `5CSEBA6U23I7`, seed 2, ordinary HeAP timing weight 10
and critical exponent 2, enable replication 4, GPU router device 1,
`--freq 74.25`, `--timing-allow-fail`, a detailed timing report and a 16,384-path
cap. The only diagnostic environment selection was
`NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY="<fresh-main-placed-guide> 64 24"`.
No reduction, remap plan, LUT-pair-copy or driver-copy option accompanied it.
The full route and RBF signoff completed normally with exit code 0 in
960.064000725979 seconds. Its 15,290 emitted paths cover the same seven windows
and endpoint set as fresh main. The recorder bound actual source/model/ELF,
inputs, command, selected environment and complete output identities before
and after the run.

The disabled run of the new ELF reproduces the whole fresh-main placed module,
ordered paired PLL records and entire pre-route timing JSON. The selected
placement changes only the 106 declared FF BELs among 14,893 original cells;
parameters, INIT, ordered ports, aliases and raw indexed consumer slots remain
exact. Both route ledgers explicitly prove each generated buffer's identity
function: 4,957 in fresh main and 4,973 after the moves. The exported physical
comparison covers 50 whole LABs, 41,410 exact outside ALMs and all 14,893
original full pin maps, with 106 changed buffer records. Forty-three unmoved
FF CLK/ENA selector rows undergo a consistent injective renumbering with
complete logical signatures and group membership preserved. ACLR index changes
are limited to unconsumed plain-FF half initialization caused by declared moves;
empty-in-both halves, L6/carry modes and LAB ACLR-used flags remain exact.
The proof binds 118 artifacts and directly rehashes 2,329 source or compiler files.

Measured source is the search checkout on base
`afd1dcf9bc38e5e168ae4e4eccabcc13e2f0c07e` plus the recorded uncommitted
three-file diff. No compiled commit is asserted. Host checks on those selected
bytes passed **33 native cases and 11 CLI methods**: nine capture-pipeline,
12 LUT-pair placement, ten LUT-driver-copy and two LAB pin-map cases. The new
fixture passed separately, followed by the 32 existing native cases and the
CLI target. The JSON binds their record, logs, production ELF, test ELF and
three measured source-file digests.

Publication updates [PR #110](https://github.com/DeanoC/nextpnr/pull/110) from
`38929152` with the exact measured production file and native fixture. Its
guide is rewritten to describe this qualified result, so the measured guide
digest does not identify the publication guide. The
[earlier record](ramtest-capture-pipeline-2026-10-02.md) preserves the earlier
16-geometry trials and budget-8 plus selected LUT-pair-copy result separately.
The frozen 117.86892700195312 MHz stack uses a different compiler, configuration
and baseline; no qualification or regression comparison is inherited from it.

The first proof attempt exited 1 solely because sorted-key manifest metadata
had a different dictionary insertion order from the rederived move rows; value
comparison had passed. It produced no proof output or accepted gain. The frozen
script and closed failure record remain retained. The corrected proof ignores
object-key order only for that manifest metadata comparison; move values and
list order, native export order, PLL records and raw-slot checks remain exact.
The final proof passed. [The JSON](ramtest-capture-search64-2026-10-02.json)
records its exact proof, recorder, run, profile, timing, RBF, prefix, manifest,
host-check and failure identities. Raw ledgers remain local and are not
committed with this summary.

This qualifies the measured ELF and explicit benchmark options. It does not
qualify a new publication binary, current default recipe or hardware. No RTL,
pins, constraints or hardware configuration changed. The physical proof checks
exported snapshots and packed functions; it does not independently reproduce
numerical STA, unreported clock-pair extrema, live pointer/cache/GPU/runtime
identity, full placement/routing/device legality, route muxes, CRAM or RBF
decode. RBF hashes identify output bytes only.
