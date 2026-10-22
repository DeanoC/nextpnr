# PC/XT ADD operand-0 with balanced overflow

The ADD operand-0 mapping regresses this paired full-route experiment. Keep the
balanced baseline as a diagnostic reference and revert the ADD mapping from the
timing-closure working recipe; preserve its implementation and proofs as failed
experimental evidence. The earlier bare ADD pair also regressed. Neither run
here passes all setup checks or endpoint coverage, and no hardware ran.

This is a separate supplement to
[the bare ADD archive](../2026-10-22-pcxt-add-operand0/README.md). Its capture SHA256
is `52004db16988c99fb4abc13e257126ae8049178a224a5210a9c7ae53cd51a4b7`.
`references.json` binds every reused source/input/proof artifact to that capture's
manifest. The prior 59 MB source/proof bundle is not duplicated here. To replay
read-only analyses outside the original worktree, extract that capture into a
sibling `add-op0-20261022` directory and this supplement into
`add-op0-balanced-20261022`; full balanced routes are under `routes/`.

The source pair is unchanged: baseline FES
`2a1cedf48efe68ccc92c0f7c7bfed52f0dc35dbd`, ADD0 candidate
`53b8fc1a60b2535aad80df22f2e3a1f1068ec507`. Both use the same locked Yosys synthesis
artifacts and frozen nextpnr binary
`e1a121d0de5e496afd96328ed435e77c0b78f913fcfd3768283c01b12313deac`, compiler commit
`a361eac060ebc25f9f70ef3bbc126814069d346b`. Both use seed 1, 100 MHz system target,
weight 2000, criticality exponent 5, hard-block windows, corrected spreading,
and the leased GPU router. QSF, SDC, PLL parameters, firmware and microcode are
unchanged. Each selects its independently proved four-cell overflow root;
selectors differ because resynthesis changed generated cell names. Exact
commands, source/tool/input identities and complete artifact hashes are in the
receipts.

| Final native setup margin | Balanced baseline | Balanced ADD0 candidate |
| --- | ---: | ---: |
| Whole system clocks[0] | -0.434 ns | -1.637 ns |
| CPU destinations, clocks[0] to clocks[0] | -0.173 ns | -1.502 ns |
| Microcode address, clocks[0] to clocks[2] | +0.124 ns | -1.317 ns |
| Microcode return, clocks[2] to clocks[0] | +1.257 ns | -1.017 ns |
| Pixel clock | +2.005 ns | +1.161 ns |
| Setup-failing endpoint rows | 14 | 211 |
| Related hold-failing rows | 0 | 0 |
| Unsupported boundaries | 105 | 105 |

Baseline's 14 setup failures are system-clock paths. Candidate has 110 system,
98 microcode-address and 3 microcode-return failures. Both native runs exit 1
and emit final analogue timing, complete routed JSON and diagnostic RBF. The
baseline's worst CPU path is DS to BP (6.831 ns routing, 2.801 ns logic); its
whole-system worst is a CGA VRAM-to-RGBI path. Candidate's worst CPU path is
`eu_alu_last_result` to DX (7.959 ns routing, 3.055 ns logic). These paths are
recorded in `route-analysis.json`; generated cell names are not treated as
stable source-bit identities.

`route-analysis.json` retains every launch/capture cohort and every endpoint
identity difference. The resynthesis pair has 17199/17202 endpoint rows, with
1389 removed and 1392 added exact keys. No common-only subset is used for a
qualification claim. Both report schema 7, `complete=false`, and 105 unsupported
boundaries. Clock/IO contracts and physical obligations match; two unsupported
reset-FF names differ. All original unsupported records are retained. External
IO, physical clock/board constraints and exact-artifact hardware acceptance
remain open.

Within each committed source build, balancing can be compared without changing
endpoint identities. `within-build-balancing-comparison.json` checks exact full
endpoint sets, launch/capture windows, timing flags, clocks, contracts and
unsupported sets before comparing margins. Balancing reduces setup failures
111 to 14 for the baseline and 366 to 211 for ADD0. It does not improve every
endpoint: all per-row setup/hold improvement and regression counts are retained.
Neither full route becomes an accepted timing winner.

The baseline's normal native guard accepts its selector on fresh raw synthesis.
`baseline-native-proof.json` proves all 131072 assignments before/after the
four-cell rewrite, private internal observability, original output identities,
and every outside function/connection/polarity/alias/top port/metadata record.
`baseline-overflow-binding.json` explicitly binds its emitted frontier to the
actual raw DX[15:0]/AX[15] FF outputs. The candidate's corresponding proof and
raw-to-native literal binding are reused by hash from the prior capture; its
exact root is recorded in its route receipt. These are function proofs, separate
from the routed timing outcome.

`routed-proof.json` reconstructs each fresh route's exact CPU-only pack/place
recipe and independently accounts for the entire final graph. Baseline has
10448 prepared cells plus 1949 fully proved route helpers; candidate has 10430
plus 1746. Every original primitive function, connection, logical pin polarity,
prepared alias class, top port, placement and nonphysical metadata is checked.
Each helper receives its own exact transparent-function, private-output,
FF-DATAIN and physical-half/pin ledger. No assumed helper matching or graph
intersection is used. Six deliberately corrupted helper/LUT mutations are
rejected. Physical input pin assignments change for 5361/5380 original cells,
with logical states unchanged.

`mapped-packed-proof.json` additionally binds all 80 ADD0 raw LUT cells to the
balanced prepared candidate through exact original FF-Q/macro-Q identities,
unchanged truth masks/connections and noninverted logical pins. It composes with
the prior actual emitted 16-lane exhaustive map proof and the complete final
route bridge. The prior extra-selector FF proof remains applicable: the three
extra candidate state bits are unobserved ADD0 selectors 13–15, with unchanged
clock/control/decode semantics. These proofs do not establish whole-board
sequential equivalence, all packing transformations, RBF bit encoding, serialized
LAB correctness, electrical routing or hardware behavior.

The committed candidate's required checks also passed: `make check` and
`make test-changed`, including the parent/component suites and all PC/XT
simulation suites. The complete parent-run logs are retained under
`validation/`. They validate the committed source, while the routed timing
reports determine that this particular mapping should be reverted.

This supplement charges only its two completed full routes:
**1781.5340479551814 seconds** = 874.1519396542571 baseline + 907.3821083009243
candidate. Earlier bare ADD routes and all other experiments are excluded.
CPU-only native exports and placement reconstruction add no full-route charge.
No additional full routes, tracked RTL/compiler edits, deployments or hardware
operations were performed for this evidence task.

`manifest.json` hashes every capture member; archive readback verifies every
member again at the final destination. Reused immutable source artifacts remain
in the explicitly identified prior archive. Native executables are not bundled.
