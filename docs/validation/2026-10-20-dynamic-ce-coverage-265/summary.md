# Dynamic active-high CEOUT reference coverage, nextpnr #265 / draft #266

The native registered-pad model now accepts active-high dynamic CEOUT on the
thirteen actual PC/XT SDRAM address/bank pins. Two bounded Quartus 17.0.2
reference fits support this extension **without changing any timing bound**.
The frozen placed-graph diagnostic gains 13 pad rows; nine reset/HPS/HDMI
boundaries remain unsupported. This is compiler/reference coverage, not a
physical PC/XT IO budget, routed timing closure or hardware acceptance.

Base: `8ee95961caf0ab649566ad36017bff5c2b04629f`.
Native implementation and test source: `b484fcbda8ce2fcf9842b1c788e3ab61662a2b3a`.
Branch: `feat/bounded-placement-swaps` in
`/home/deano/harnesses/pcxt-compiler-repair/nextpnr`.
Only new local commits; no push, merge, FES edits, hardware or subagents.
Parent owns independent audit and publication.

## Reference evidence

`reference-fits.tar.gz` retains both complete fitted projects: RTL, QSF, SDC,
Quartus databases, RBF/SOF, decoded configuration, compile logs, all raw paths,
clock queries and declarations, parsed evidence and SHA-256 identities.
`tool-provenance.json` binds the Quartus executables and exact decoder binary,
library, helper source and helper binary. The decoder comes from a retained
unversioned source snapshot; no unverified Git identity is claimed for it.
The native build's separate Mistral source identity is recorded in the manifest.

Both fits use `5CSEBA6U23I7`, the actual thirteen package pins, 3.3-V LVTTL,
16mA, fast slew, D5=0, and declared reference loads of 0 and 30pF. The source
counter supplies varying data and a varying active-high enable. The fits
retain all thirteen output registers in IO hardware. Decoded CEOUT routing,
`CE_OUTREG_TIEOFF_EN=1`, SDR mode, zero selectors and direct inverter queries
prove hardware CE control with inversion=0; it was not replaced by a data mux.

Every pad has explicit rise/fall output queries, early/late paths and all four
slow/fast temperature corners. Data `|d` and control `|ena` are classified by
the actual DDIO ingress, with setup and hold for every pin/corner. Local fabric
checks subtract the internal clock CELL from the data CELL and include the
intrinsic check. Pad paths start at the register clock routing ingress and
include its local clock CELL and complete register-to-buffer-to-pad path.
Upstream clock routing is excluded. Primitive pulse requirements include local
late-minus-early distortion, without importing global routing as a local arc.

| Channel | Observed maximum | Unchanged native envelope |
| --- | ---: | ---: |
| Pad late, 0pF | 4100ps | 0..5280ps |
| Pad late, 30pF | 5239ps | 0..5280ps |
| CE setup / hold | 55 / 270ps | 120 / 310ps |
| Data setup / hold | -28 / 308ps | 120 / 310ps |
| Primitive period / ingress high / ingress low | 1538 / 790 / 767ps | 1540 / 790 / 770ps |

The early bound remains conservative zero. These reference loads are not
measurements of the user's board. No load interpolation or routing coefficient
was introduced. `reference_check.py` reparses raw reports, verifies every file
hash and checks complete pin/corner/channel coverage. `reference_negative.py`
rejects eight independent corruptions, updating relevant hashes so semantic
coverage and configuration guards are exercised.

The native extension requires standalone `MISTRAL_SDROUT`, one of the thirteen
qualified pins with matching BEL, a driven nonconstant CEOUT signal, PIN_SIG,
IOREG_OUT_CE=1, IOREG_CEOUT_INV=0, connected clock and inactive output clear.
All existing profile/electrical/load/clock guards remain. Missing CE with an
enabled hardware parameter also rejects. Other pins, dynamic CEIN, bidirectional
and DDR modes, inverted controls, undriven controls and nondefault selectors
remain unqualified. Native data and CE setup/hold endpoints remain distinct.

## Frozen topology comparison

Original checkpoint SHA-256:
`733f1cc8ea765bed46dae44dcd744e4432edd75ddedaf232061c0a1d9756b4a6`.
The new overlay and synthetic SDC are separate files in the output root;
no original or previous overlay/checkpoint was modified. Profile and unmeasured
30pF load declarations are applied to the 21 accepted physical pads. External
-1..2ns bounds remain arbitrary host-test budgets.

| Diagnostic | Dynamic rows | Static proofs | Unsupported |
| --- | ---: | ---: | ---: |
| Previous eight-pad reference overlay | 17233 | 1888 | 22 |
| New dynamic-CE overlay | 17246 | 1888 | 9 |

`check.py` compares all previous rows, static proofs, existing registered-pad
contracts and primitive clocks exactly. Original and synthetic snapshots match
the previous evidence exactly. All 26 existing I/CEOUT rows on the newly
accepted pads are individually preserved; the C12 CE setup margin remains
209ps in this placed diagnostic. `changed-identities.json` retains each new
cell/pin/channel/row, and `remaining-endpoints.json` plus `remaining-details.json`
retain the nine exact open boundaries: four reset ACLRs, one HDMI clock and
four HPS I2C data/OE endpoints. No matching-unsupported waiver was added.
All thirteen newly exposed pad rows fail the arbitrary synthetic setup budget
(minimum setup margin -4620ps); minimum hold margin is 3680ps. Their existence
is coverage, not a timing pass. The previously rejected route candidate still
has 15 new setup failures.

## Validation and reproduction

The CPU-only Release build used ARCH=mistral, GPU_ROUTER=OFF, BUILD_TESTS=ON,
GUI/Python off and IPO on. Native binaries were built from committed b484fcbd,
copied to `final-bin`, made nonwritable and hashed before tests. No tested binary
was relinked. Source and binary identities are in `manifest.json`.
Final native `IoDelayTest.*:CriticalCohort.*`: **88/88 passed**.
Driver: **21/21 passed**. The manifest and logs retain these results.
All six previous portable checkers passed, including the unchanged historical
route rejection. Reference semantic rejection tests passed 8/8.

From the repository root, the portable audit needs only committed evidence:

```sh
python3 docs/validation/2026-10-20-dynamic-ce-coverage-265/check.py
```

To repeat the bounded reference fits in a fresh output directory:

```sh
CE_DOC=docs/validation/2026-10-20-dynamic-ce-coverage-265
python3 "$CE_DOC/prepare.py" /new/reference-output
# In each reference0 and reference30 project, using Quartus 17.0.2:
quartus_sh --flow compile top > compile.log 2>&1
quartus_sta -t paths.tcl > paths.log 2>&1
quartus_sta -t clock_requirements.tcl > clock.log 2>&1
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
# Compile the committed helper against the same decoder library/database:
# MISTRAL_SRC is its source directory; MISTRAL_BUILD contains generated tools/.
c++ -std=c++17 -I"$MISTRAL_SRC/libmistral" -I"$MISTRAL_BUILD/tools" \
  /absolute/repo/docs/validation/2026-10-20-dynamic-ce-coverage-265/control_decode.cc \
  /path/to/libmistral.a -llzma -o /new/control_decode
/new/control_decode output_files/top.rbf > ce-inverters.tsv
# Then from the repository root:
python3 "$CE_DOC/extract_reference.py" /new/reference-output
python3 "$CE_DOC/reference_check.py" /new/reference-output --output /new/reference-receipt.json
python3 "$CE_DOC/reference_negative.py" /new/reference-output
```

To repeat final native validation with the frozen local binaries:

```sh
CE_ROOT=/home/deano/harnesses/pcxt-dynamic-ce-coverage
"$CE_ROOT/final-bin/nextpnr-mistral-test" --gtest_filter='IoDelayTest.*:CriticalCohort.*'
PYTHONDONTWRITEBYTECODE=1 python3 mistral/tests/routed_swap.py "$CE_ROOT/final-bin/nextpnr-mistral-test"
NEXTPNR_TEST_IO_CHECKPOINT=/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json \
NEXTPNR_TEST_IO_SDC="$CE_ROOT/synthetic-io.sdc" \
NEXTPNR_TEST_REGISTERED_OVERLAY="$CE_ROOT/reference-profile-overlay.json" \
NEXTPNR_TEST_REGISTERED_SDC="$CE_ROOT/registered-synthetic.sdc" \
NEXTPNR_TEST_IO_DIAGNOSTIC=/new/path/dynamic-ce-host.json \
  "$CE_ROOT/final-bin/nextpnr-mistral-test" \
  --gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints
```

Two bounded primitive reference fits ran; **zero PC/XT route invocations** ran.
The PC/XT campaign still has **1627.843715112 seconds** of its original route
budget. Remaining endpoint coverage, evidence-backed physical IO budgets,
100MHz setup/hold qualification, seed gate, fresh producer/exact-artifact
qualification and separately authorized hardware acceptance remain open.
