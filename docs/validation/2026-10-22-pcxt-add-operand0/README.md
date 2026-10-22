# ADD operand-0 board synthesis comparison

This directory prepares fresh board synthesis from two committed FES source
snapshots, using the same explicitly selected, hashed locked Yosys. It is a
host diagnostic and does not invoke or claim authenticated producer admission,
routing, timing qualification, package sealing or hardware acceptance.

`baseline` starts at commit `2a1cedf48efe68ccc92c0f7c7bfed52f0dc35dbd`.
`candidate` must receive the independently reviewed source commit explicitly;
it never reads the mutable source worktree. Both use CGA Direct, the same
100 MHz system/CPU target and actual +90 degree 100 MHz microcode read clock.
The board helper's normal `board_inputs()` produces the firmware and ROM lane
files and checks imported source hashes. Its actual `yosys_program()` and
`synthesis_command()` supply the complete board synthesis sequence.

Run from the task worktree:

```
python3 out/add-op0-20261022/synthesize.py baseline --commit 2a1cedf48efe68ccc92c0f7c7bfed52f0dc35dbd
python3 out/add-op0-20261022/synthesize.py candidate --commit 53b8fc1a60b2535aad80df22f2e3a1f1068ec507
```

The script refuses to overwrite either run. Each run retains:

- `source.tar.gz` and `source/`: complete committed misteross source snapshot.
- `snapshot.json`: commit/tree, archive command/hash and every source-file hash.
- `inputs.json`: exact selected board inputs, generated firmware/ROM/constraints,
  Yosys binary/version/library hashes and assembler/linker identities.
- `synthesis/synth.ys`, `yosys.log`, `synth.json` and all generated inputs.
- `receipt.json`: exact command, exit, elapsed time and every output hash.

Yosys binary SHA256 is
`71eb21e86017064c141bca25e0ecd7fd39dcb3671f3d560fbc467eb880b2a650`.
The script checks sources, binary and complete Yosys share-library hashes again
after synthesis. No graph differences are discarded or normalized for retention;
both complete output graphs are kept. Lower LUT depth alone does not qualify
setup, hold or endpoint coverage. The synthesis comparison must precede a
separate frozen paired full-route experiment and the remaining acceptance gates.

The completed candidate is commit
`53b8fc1a60b2535aad80df22f2e3a1f1068ec507`. Both synthesis runs exited zero.
Baseline/candidate synthesis took 314.2237433027476 / 758.4593265280128 seconds.
The selected Yosys, all 327 share-library files, assembler/linker, firmware,
ROM lanes/facts, QSF and SDC are identical. `committed-source-review.json` and
`source-change.patch` account for all 12 committed files; all nine previously
validated implementation-file hashes match their committed bytes.

The complete raw inventories change from 10498 to 10480 cells, 7471 to 7437
named nets, and 3740 to 3743 flip-flops; both retain 318 M10Ks. Global synthesis
remaps and renames logic, so this is not an 80-cell-only whole-board graph edit.
`comparison.json` retains the full inventory difference. The candidate's actual
ADD0 implementation is 64 ALUT6 product cells plus 16 ALUT4 OR cells: five cells
per lane and two LUT levels. `mapped-proof.json` exhaustively checks 16777216
control/data assignments for each of 16 lanes, including zero and multi-hot
controls. `mapped-packed-proof.json` binds all 80 actual cells through packing
by their exact original FF-Q/macro-Q drivers, unchanged masks/connections and
noninverted logical pins; source aliases consolidated during import are listed.

The three extra FFs are `eu_add_select0[13:15]`. Baseline aliases are `x`;
candidate outputs have no cell or top-port observers. `selector-proof.json`
checks every retained selector against all 16 values of actual
`data_half[23:20]`: its DATAIN is exactly equality to that selector index,
CLK is `clocks[0]`, ACLR=1, ENA=1, SCLR=0, SLOAD=0 and SDATA=0. Bits 13/14 are
masked off; source 15 is constant zero. These are retained unused state from
already-existing kept selector processes, with no consumer-visible pipeline
stage. The source helper is combinational and source clocks/reset/latency,
ROM bytes and override logic are unchanged.

`ff-polarity-alias-pairing.json` accounts for 3075 unambiguous state pairs by
actual RTL aliases, including explicit inversions. `ff-audit.json` matches
another 131 generated pairs by their actual input Boolean functions at those
anchors. `cpu-generated-ff-proof.json` additionally proves the one renamed
EU generated-state pair's complete input/control functions, including carry
logic, under its state correspondence. The remaining 533 generated state
pairs outside that EU scope have not received a complete sequential matching
proof. Every raw cell remains available; no unchanged whole-board sequential
behavior is claimed from inventory counts or the local helper proof.

The bare ADD experiment used frozen nextpnr binary SHA256
`e1a121d0de5e496afd96328ed435e77c0b78f913fcfd3768283c01b12313deac`, recorded source
commit `a361eac060ebc25f9f70ef3bbc126814069d346b`, seed 1, the same input constraints,
100 MHz target, timing weight 2000, criticality exponent 5, hard-block windows
and corrected spreading. It used the leased GPU router. Each receipt contains
the exact command, source/binary/input identities and every output hash.
Neither command selected an overflow balancing root.

| Final native setup margin | Baseline | ADD0 candidate |
| --- | ---: | ---: |
| Whole system clocks[0] | -0.967 ns | -2.227 ns |
| CPU destinations, clocks[0] to clocks[0] | -0.967 ns | -2.032 ns |
| Microcode address, clocks[0] to clocks[2] | -0.199 ns | -2.023 ns |
| Microcode return, clocks[2] to clocks[0] | +0.915 ns | -0.391 ns |
| Pixel clock | +2.354 ns | +1.735 ns |
| Setup-failing endpoint rows | 111 | 366 |
| Related hold-failing rows | 0 | 0 |
| Unsupported boundaries | 105 | 105 |

Both full routes exited 1 after emitting final analogue reports, routed JSON
and diagnostic RBF. The candidate regresses this recipe and is not an accepted
timing winner. `route-analysis.json` keeps every clock pair and the worst
reported CPU/other paths. Baseline's worst CPU path is DX to a microcode address
register (7.913 ns routing, 2.527 ns logic); candidate's is DI to DX (8.161 ns
routing, 3.331 ns logic). Candidate's worse whole-system path is CGA VRAM to
RGBI, separate from its CPU margin.

Native endpoint identities number 17199/17202. Exact key inventories contain
1389 removed and 1392 added rows as generated identities change; no intersection
is used as qualification. Clock/IO contracts and physical obligations match.
Unsupported inventories contain 105 rows each, but two generated reset-FF
names differ; their complete records remain in the reports. Both reports have
`complete=false`. This does not establish external IO coverage, board timing,
electrical RBF behavior or hardware acceptance.

`routed-proof.json` independently bridges each exact fresh placement recipe to
its own final route: baseline 10448 prepared cells plus 1848 route helpers;
candidate 10430 plus 1808 helpers. Every original function, connection,
logical pin polarity, prepared alias class, top port, placement and nonphysical
metadata is checked. Each helper is proved as an exact noninverting private FF
DATAIN path with its own physical-half/pin ledger; unmatched helper identities
are not assumed equivalent. Six deliberately corrupted helper/LUT mutations
are rejected. Physical input assignments change for 5378/5314 original cells,
with logical states unchanged. This bridge does not prove whole-board packing,
RBF bit encoding or electrical routing.

The two already-completed full routes charged **829.44478589249777 seconds**
(380.52875910466537 baseline + 448.9160267878324 candidate). This subtotal excludes
all earlier and subsequent routes. CPU-only synthesis, native exports and
placement reconstruction add no full-route charge. No additional full routes
or hardware operations were performed by this evidence task.

An exact semantic overflow search is also retained for the next paired
experiment. The baseline preserves the original four-cell overflow root;
that generated name is absent in the candidate. `overflow-selection-proof.json`
binds the only candidate four-cell root implementing the same actual
DX[15:0]/AX[15] all-zero condition:

```
system.machine.cpu.cpu.u_eu_core.idiv_opcode_MISTRAL_ALUT5_A_E_MISTRAL_ALUT5_Q_1
```

The frozen compiler accepts this selector under its normal guards in an
unpacked/unplaced/unrouted export. `overflow-native-proof.json` independently
checks all 131072 assignments before/after balancing, exact private internal
observability, all four original cell/output identities, every outside
function/polarity/connection, aliases/metadata/top ports and all root observers.
Its cross-resynthesis observers are not claimed identical to the baseline's.
These exports are preparation evidence only; later balanced full routes are
separate experiments and are excluded from this archive and route subtotal.

`capture-source/` selects the immutable source archives, full synthesis and
route artifacts, input/command receipts, focused validation logs and all final
proofs. `manifest.json` hashes each member; the archive is read back completely
before delivery. Source snapshots can be restored from each `source.tar.gz`
into its run directory to rerun scripts that expect `source/`. No executable
compiler binaries or duplicated expanded source trees are bundled.
