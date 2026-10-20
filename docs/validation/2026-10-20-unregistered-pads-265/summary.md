# Explicit unregistered SDRAM DQ pad references — nextpnr #265

The compiler adds opt-in data/OE tails and input prefixes on all sixteen actual
SDRAM DQ pins. Four valid Quartus17.0.2 small fits cover four corners and both
transitions at0/30pF. Two targeted RCF fits use every exact native GIN; earlier
272 input observations on different GINs remain explicitly excluded. The audit
retains2800 matching paths. This is conditional reference coverage, not PCXT
physical timing closure or board qualification.

Implementation source: `03cf9cdce89e2d4fa667e1f760575fc4b09afc71`.
The final CPU-only binaries carry version`03cf9cdc`, were copied into
`/home/deano/harnesses/pcxt-pad-models/final-bin`, made nonwritable and hashed
before final tests. They were not relinked after testing. `source-identity.json`
records source/tree, implementation-file hashes, native Mistral identity,
build configuration and both final binary hashes.

## Explicit new configuration

The timing profile is `QUARTUS_17_0_2_DQ_UNREGISTERED`, with declared0..30pF,
3.3-V LVTTL,16mA,fast slew, unregistered bidirectional IO and native zero
D1/D3/dataD5 selectors plus OED5=31. It requires a separate explicit
`NEXTPNR_INPUT_REG4_SEL=BYPASS` hardware option. The old native emitter selects
`LOCKED_DPA`; the database documentation does not prove that selector inactive
on these paths. No equivalence or applicability to old native RBFs is claimed.

In the isolated native pad-emission fixture, the timing annotation alone
leaves RBF bytes identical. The separate BYPASS option changes exactly sixteen
PRAM selector bits, one per DQ lane; all sixteen
`INPUT_REG4_SEL` values change from`SEL_LOCKED_DPA` to`SEL_BYPASS`. Native emitted
selectors match the fitted configurations. Invalid values/types, missing
configuration, absent output-enable/emission branch and mismatched timing
profiles reject. `native-*.rbf.gz`, `selector-delta.*` and `native-config.json`
retain this distinction. The portable reference checker also compares the
compiled model intervals against independently extracted report extrema.

## Native boundaries and model arithmetic

`boundaries.txt` lists all48 actual pin/port/routing identities. Data and OE are
separate GOUT/LEIM ingress points. HMC bypasses are retained where present.
The tail begins after that ingress, including the subsequent HMC and pad
buffer/load delay. Input prefixes end before GIN/VIO driver delay: native
`analogue_arc_delay` simulates that driver on its first outgoing hop. Including
the VIO increment in the prefix would double-count it.

`reference.json` contains per-pin/per-channel rising and falling late bounds,
rounded outward to10ps, with a conservative zero early bound. All delays are
separate logic segments in reports; saved IO budgets and native route segments
are unchanged. OE rise/fall paths do not independently characterize high-Z
disable thresholds or contention-free turnaround, including SDRAM tHZ/tLZ.
KitA MiSTerPi and KitB SuperStationOne use standard MiSTer SDRAM, but actual
chip/board delays and load are unknown. None of these reference loads or the
synthetic fixture budgets are claimed as measured board parameters.

Per-pin late envelopes below take the maximum of the independently retained
rising/falling bounds. Every early bound is zero.

| DQ bit | Pin | Data late (ns) | OE late (ns) | Input late (ns) |
|---:|---|---:|---:|---:|
| 0 | V12 | 5.000 | 6.280 | 2.890 |
| 1 | E8 | 5.270 | 6.330 | 2.820 |
| 2 | D11 | 5.250 | 6.430 | 2.740 |
| 3 | W12 | 5.100 | 6.390 | 2.730 |
| 4 | AH13 | 6.910 | 8.000 | 5.140 |
| 5 | D8 | 5.230 | 6.330 | 2.800 |
| 6 | AH14 | 6.900 | 6.310 | 2.780 |
| 7 | AF7 | 6.940 | 6.310 | 2.810 |
| 8 | AE24 | 6.810 | 7.950 | 5.220 |
| 9 | AD23 | 6.810 | 7.940 | 2.970 |
| 10 | AE6 | 5.350 | 6.400 | 2.840 |
| 11 | AE23 | 6.910 | 8.060 | 5.060 |
| 12 | AG14 | 6.940 | 6.310 | 2.810 |
| 13 | AD5 | 5.390 | 6.390 | 2.870 |
| 14 | AF4 | 5.090 | 6.320 | 2.790 |
| 15 | AH3 | 6.960 | 6.330 | 2.830 |

## Preserved host coverage

All prior snapshots are identical as JSON values except schema7 and its
new explicit empty`unregistered_contracts` field. The previous final17246 rows,
1888 static proofs, clock contracts, reset/forwarding/constant proofs, primitive
checks and synthetic IO budgets remain intact.

The separate`unregistered` snapshot retains17246 rows and1888 proofs. Exactly48
external endpoints change:32 data/OE and16 input. Only max-path/setup change by
equal positive/negative model additions; hold/min-path and all prior contracts
remain unchanged. It records48 named reference models and52 physical obligations.
Both HPS`scl.OE`/`sda.OE` endpoints remain unsupported, and`complete=false`.
The schema7 retention driver rejects these unqualified models even if completion
and physical-obligation fields are forged. Historical whole-design snapshots
are never promoted to current acceptance. `prior-gates.py` adapts only isolated
host fixtures while checking original immutable evidence hashes.

## HPS I2C investigation

The one small HPS fit preserves numerical output CELL observations at all four
corners and both transitions. They are not clock-to-output launch bounds.
Pseudo-registers`i2c~FF_4791` and`i2c~FF_4792` share internal source`i2c~FF_4397`;
that source has no synchronous predecessor. All three expose zero period/pulse
declarations, and`report_clocks` reports no clocks. There is no usable qualified
peripheral-clock relationship to the fabric clocks. No fabric launch, falsepath
or zero-delay hard-IP launch was invented. `hps-reference.json`, `hps_check.py`
and the raw fitted project retain the reproducible evidence.

## Validation and failures

Final native suite:95 tests; retention driver:24 tests; host diagnostic:1 test.
Six reference corruptions reject. All eight prior portable evidence gates pass.
The exact final test receipts and source/binary identities are retained.

The small all16 DQ synthetic CLI trial failed input hold timing: its unchanged
0..1ns external arrival and conservative zero pad minimum do not cover the
physical capture-clock latency. The route exits1; a no-route reload also exits1
and reproduces identical RBF and full timing report bytes. No constraints,
acceptance gate or search budget was relaxed. These files remain nonpassing
artifacts, not a qualified route. The initial positive-trial expectation failed;
the retained replay establishes deterministic rejection, not timing closure.

Seven of at most eight new fits were used, each bounded600s: two rejected fits
with malformed braced QSF node names, four valid DQ fits, and one HPS I2C fit.
The first targeted30 path query overlapped the fit's own TimeQuest work and
hit its120s limit. Its partial capture is retained separately. The same fitted
artifact was queried again only after automatic STA finished, under a600s
bound; this completed in2s. No additional fit or seed reroll occurred.
`fit-attempts.json` and `failed-attempts.tar.gz` preserve these outcomes.

## Portable checks and reproduction

From a fresh Git archive of this branch:

```sh
python3 docs/validation/2026-10-20-unregistered-pads-265/check.py
```

The checker verifies archive/report/configuration hashes, all native boundary
models and selectors, the host preservation/deltas, HPS gap, exact selector
scope, nonpassing CLI result, and the retained final test receipts. It requires
no local cache, Quartus install, frozen binaries or hardware.

To repeat the host diagnostic with a rebuilt or retained matching binary:

```sh
python3 docs/validation/2026-10-20-unregistered-pads-265/portable-host.py \
  /path/to/nextpnr-mistral-test /new/host.json
```

`host-inputs` includes the frozen placed checkpoint and every overlay/SDC.
The original checkpoint is decompressed to a temporary directory and never
edited. To repeat vendor queries, extract`reference-fits.tar.gz` into a new
working directory and run Quartus17.0.2`quartus_sta -t paths.tcl` in each fitted
project after any compilation finishes. The source fixture README explains
bounded fit and RCF reproduction. `run-pad-cli.py` reproduces the small negative
CLI case from the extracted reference root; it is not a PCXT route.

**Zero PCXT route invocations. No GPU, hardware, FES edits, push or merge.**
Remaining PCXT route budget is unchanged at**1627.843715112 seconds**. Parent
publishes and closes this worker. Board budgets, high-Z turnaround, HPS launch,
qualified fast-corner routing,100MHz routed setup/hold, the seed gate, fresh
producer/exact-artifact qualification and hardware acceptance remain open.
