# GPIO register timing characterization

`characterize.py` compiles the existing SDR/DDR/OE register fixtures in Quartus
17.0.2 Lite for 5CSEBA6U23I7 and 3.3-V LVTTL. It retains their RTL, QSF, SDC,
fitted databases, complete setup/hold paths, and hashes. TimeQuest queries all
four available slow/fast temperature corners. These are reference fits with
synthetic external constraints, not SDRAM constraints or hardware signoff.

```sh
python3 mistral/tests/gpio-timing/characterize.py \
  --quartus-bin /path/to/quartus/bin --output /tmp/gpio-timing
python3 mistral/tests/gpio-timing/summarize.py /tmp/gpio-timing/evidence.json \
  --output /tmp/gpio-timing/reference.json
```

`--reuse-fits` queries existing fits only after checking their source and
assignments; Quartus's `LAST_QUARTUS_VERSION` metadata and blank lines are
ignored when checking QSF identity. Changing an assignment still fails.

The adjacent reference receipt records observed maxima, their paths and input
hashes. Local register setup is the data CELL delay less the internal clock
CELL delay, plus intrinsic setup. Hold reverses the subtraction. Input
clock-to-fabric combines the internal register clock CELL and output CELL
paths. Clock-buffer, interconnect, package and output-buffer delays are excluded
from these local arcs. The backend uses maxima rounded outward to 10 ps and a
conservative zero clock-to-Q minimum. These reference envelopes do not certify
all silicon, placements, electrical conditions, or fast-corner routing delay.

The current backend profile covers fabric-facing SDR/DDR input outputs, input
clock enables, and registered SDR output/OE data and enables on the
tested device with default delay settings. Both DDR outputs launch into the
fabric on the rising clock edge; the low word's falling pad-capture edge is a
separate relationship. Asynchronous clear stays uncharacterized. Other devices,
LVCMOS, bus hold, and explicit delay-chain settings retain their unsupported
classification.

Output-only `MISTRAL_SDROUT` now keeps the database's zero delay-chain defaults,
matching these Quartus fits. Explicit `D5_DELAY` assignments still override
the default and retain their unsupported fabric timing classification.
`compare_sdrout.py` audits a second fit of `pads.v` with `D5_DELAY 31` on p4/p6.
Its QSF also requests `D5_OE_DELAY 31`; Quartus reports that the OE delay is
unused on those output-only pins and resets it to zero. The audit checks source
and SDC identity, permitted QSF differences, decoded data-delay selectors,
equal reported clock paths, and matched setup/hold paths at all four corners.

```sh
python3 mistral/tests/gpio-timing/compare_sdrout.py \
  --baseline /tmp/gpio-timing/pads --delayed /tmp/pads-forced31 \
  --output /tmp/sdrout-delay-comparison.json
```

The adjacent `sdrout-delay-reference.json` receipt records sixteen matched
clock-to-pin observations and 24 unchanged fabric setup/hold checks. Forced
D5=31 adds 0.483–1.180 ns in those fits. Removing that delay improves setup
but also reduces output hold margin; board timing must determine any required
explicit compensation. This is not evidence of the cause of issue #135, or
proof that either setting passes SDRAM hardware.

External pad capture and output timing remain unsupported. The current IO-delay
analyzer refuses registered pads rather than silently timing only their fabric
side. The common analyzer now supports independent read/write boundaries,
separate capture edges and data/OE channels through an architecture API, with
synthetic-model regression coverage. Mistral does not supply that API's pad
models yet. Completing its support requires DDR handoff coverage, matching
output delay-chain settings, and a qualified clock-to-pin/output-load model.
Some internal DDR handoff queries report no paths; that absence is not proof of
zero delay or a passing half-cycle check.

## Complete pad reference paths

`pad_summary.py` extracts pad capture setup/hold and registered data/OE
clock-to-pin observations from retained path reports. Capture includes the
input buffer and all local data delays, subtracting the register's internal
clock CELL delay. Hold reverses that subtraction. Output combines the local
register clock CELL with all register-to-pin delays, including the output
buffer. The upstream clock network ends at the register clock ingress and
is excluded from these arcs, so routed clock delay is not counted twice.
DDR rising and falling capture remain separate; this extraction does not
qualify the internal low-word handoff to the rising fabric output.

```sh
python3 mistral/tests/gpio-timing/pad_summary.py /tmp/gpio-timing/evidence.json \
  --output /tmp/pad-reference.json
python3 mistral/tests/gpio-timing/check_pad_summary.py /tmp/gpio-timing/evidence.json
python3 mistral/tests/gpio-timing/characterize.py \
  --quartus-bin /path/to/quartus/bin --output /tmp/gpio-load30 \
  --variants ddr pads --output-load-pf 30
python3 mistral/tests/gpio-timing/compare_load.py \
  --baseline /tmp/gpio-timing --loaded /tmp/gpio-load30 \
  --output /tmp/load-comparison.json
```

The load option requests a lumped far-end capacitance on the named pads using
`BOARD_MODEL_FAR_C`; it is a reference sweep, not an estimate of the user's
board. Quartus 17.0 TimeQuest ignores the obsolete `OUTPUT_PIN_LOAD` option.
The assignment uses farads as documented in the
[Quartus 17.0 Far capacitance option](https://www.intel.com/content/www/us/en/programmable/quartushelp/17.0/logicops/logicops/def_board_model_far_c.htm).
The original reference fits have no added external capacitance. A zero-load
reference cannot establish the maximum output delay for a real memory board.
The extractor checks clock ingress, complete input/output buffers, accumulated
delay consistency, both setup/hold or early/late observations, and all four
corners. Its regression check rejects missing buffers, missing clock cells,
altered delay totals, conflicting capture edges and missing corner/check types.
The resulting receipts record observations and hashes, without enabling
production pad timing in Mistral.

The loaded SDR/DDR-input reference receipts cover eighty matched registered
output data/OE paths. A lumped 30 pF load adds 0.487–1.222 ns relative to the
unloaded fits, reaching 5.414 ns clock-to-pin in the worst OE observation.
The comparison checks equal RTL/SDC, only capacitance assignment changes,
matching register channels and equal local register clock delay. Both bounds
and per-path load differences retain report/evidence hashes. The read setup
observations reach 6.351 ns relative to the clock routing ingress; negative
hold requirements are retained rather than clamped to zero.

`--variants ddr-output-data` additionally characterizes the existing varying-data
DDR-output oracle fixture, including its two output phases and fabric register
checks. It uses a synthetic 10 ns clock for characterization, not the oracle's
original 20 ns constraint. Run it separately for the default and loaded cases;
the same pad extractor and load comparison apply. TimeQuest represents the
DDIO output clock mux transfer as a data path from the clock port, not a
register clock-to-Q arc. The extractor stops the upstream clock network at
that mux's routing ingress and keeps the rising and falling phases separate.
Sixteen matched observations add 0.558–1.221 ns with the 30 pF load, reaching
5.461 ns. This fixture uses PIN_W15; its bounds do not qualify the actual
SDRAM clock pin or all placements. Internal DDR data-register setup/hold,
low-word handoff and asynchronous controls also need separate coverage.

The adjacent `pad-reference0.json`, `pad-reference30.json` and
`pad-ddr-reference{0,30}.json` receipts record the observed extrema, counts,
corner/report provenance and input hashes. `pad-load-reference.json` and
`pad-ddr-load-reference.json` retain all matched load differences and evidence
hashes. These data remain distinct from the existing production fabric arcs.

## Actual SDRAM pin and transition coverage

`--ddr-output-pin AD20` moves the DDR-output fixture to the actual SDRAM clock
pin. `--all-transitions` additionally queries each rising/falling pad transition
and audits both explicit queries for every clock phase, endpoint and corner.
Reusing a fit discards its old path reports before querying, so reports from
previous options cannot satisfy that coverage check.

`--variants ramtest-pads --all-transitions` covers all 39 SDRAM pin locations
from the retained FES DE10-Nano 100 MHz QSF: sixteen DQ pads with DDR capture,
registered data/OE, and registered address, bank, command and mask outputs,
plus the DDR-output clock pad on AD20. Its clock-pad data varies to exercise
both transitions at each clock phase; this is not a functional SDRAM tester.
Its output/register choices and synthetic single 10 ns clock differ from the
real core, including its masks and separate PLL/capture clocks.

```sh
python3 mistral/tests/gpio-timing/characterize.py \
  --quartus-bin /path/to/quartus/bin --output /tmp/ramtest-pads30 \
  --variants ramtest-pads --all-transitions --output-load-pf 30
python3 mistral/tests/gpio-timing/pad_summary.py /tmp/ramtest-pads30/evidence.json \
  --output /tmp/ramtest-pad-reference30.json
```

The complete pin reference raises observed input setup to 6.433 ns (rising
capture) and 6.421 ns (falling capture). With 30 pF added load, observed maxima
are 5.276 ns for SDR data, 5.416 ns for OE and 5.391/5.372 ns for the DDR clock
pad's rising/falling launch phases. The matched load audit covers 1,344 paths
including worst, rise and fall queries. The existing input clock-to-fabric
envelope still covers the 866 ps maximum in 256 fabric observations. Compact
load receipts (`compare_load.py --compact`) retain counts, extrema witnesses
and evidence hashes without duplicating every path.

## Declared primitive clock checks and DDR abstraction

Run the clock query from a retained fitted project directory, then audit it:

```sh
/path/to/quartus/bin/quartus_sta -t /absolute/path/to/clock_requirements.tcl
python3 mistral/tests/gpio-timing/clock_summary.py /tmp/gpio-timing/ddr \
  --output /tmp/clock-reference.json
python3 mistral/tests/gpio-timing/check_clock_summary.py /tmp/gpio-timing/ddr
```

The query retains each register's declared minimum period and high/low pulse
width, synchronous input edges and data fanouts. Its separate minimum-pulse
reports use a dedicated parser. The audit compares declarations with every
reported check, verifies actual-minus-required slack, requires all four
corners and rejects missing checks or inconsistent local clock paths.

TimeQuest exposes the DDR input's falling capture and retimed rising low-word
output as separate nodes with no connecting data edge. The receipt records
this opaque relationship; it does not invent a zero-delay handoff arc. The
39-pin fits expose 120 registers and 1,440 matching clock checks across four
corners. DDR primitive period requirements reach 1.538 ns. Pulse requirements
at internal registers must be normalized to the physical GPIO clock ingress:
the audit adds late-minus-early local clock CELL delay, swaps high/low for an
inverted internal clock, and excludes credit from global clock pessimism
removal. Observed ingress requirements reach 167/185 ps for DDR input and
806/778 ps for registered OE high/low widths; data output reaches 790/767 ps.
These are local requirements. The timing engine now checks period and pulse
widths for the explicit pad reference profile, conservatively subtracting the
routed clock delay range from each pulse. External uncertainty still belongs
in the physical clock waveform and board budgets.

`ramtest-pad-reference{0,30}.json` and `ramtest-pad-load-reference.json` retain
the complete pin/transition extrema and compact matched-load audit.
`ramtest-clock-reference.json` retains the loaded fit's declared and normalized
clock requirements, opaque DDR groups and hashes. The GPIO requirement extrema
are identical between the default and 30 pF fits. The clock audit's seven
negative cases cover missing corners/checks, declaration mismatches, inconsistent
slack, unknown check types, a new explicit handoff edge and missing local clock
cells. The backend uses outward-rounded pad and clock envelopes from these
receipts only when `NEXTPNR_GPIO_TIMING_PROFILE=QUARTUS_17_0_2_RAMTEST` is
explicitly assigned to a supported pad. See
[the profile contract](../../../docs/mistral-io-delay.md#gpio-model-boundary)
and `../registered-pad/check.py` for the native packing/checkpoint regression.
The profile does not establish board timing or prove 100/130 MHz hardware
operation. Fabric-data DDR output remains unsupported for external pad
constraints.

## Constant clock forwarding at AD20

`--variants clock-forward clock-forward-inverted --all-transitions
--output-load-pf 30` fits both constant forwarder polarities at the actual
SDRAM clock pin. `clock-forward-reference30.json` retains their four-corner
mux/pad observations. Their late bounds reproduce the earlier varying-data
reference: 5.391ns for fabric rising and 5.372ns for falling launches. Constant
forwarders have one possible pad transition per launch phase; the audit checks
that transition against declared polarity and requires its explicit query.
Changing the polarity declaration or deleting transition evidence rejects.

`check_mux_clock_frame.py <ramtest-evidence.json> <ramtest-project>
--check-rejections` audits the clock reference frame. It compares each early/
late register clock prefix with the corresponding DDR mux path, requiring
matching nodes, transitions and delays up to the shared ingress. All 32
prefixes match across four corners at both reference loads. The normalized
AD20 register pulse requirements reach 759/733ps; the backend conservatively
uses the wider existing output family envelope of 790/770ps and period1540ps.
`clock-mux-frame-reference.json` retains counts and source/report hashes. Four
negative cases cover modified delays, nodes, transitions and missing edges.

Only constant forwarders at AD20 are enabled in the explicit reference
profile. Both pad phases and their native clock routes remain visible to STA.
This does not create a generated clock on the output pin or certify the SDRAM
chip's clock waveform. See `../registered-pad/check_clock.py` for native
normal/inverted forwarding, primitive guards and checkpoint replay.


## SDR capture on the actual SDRAM pins

`--variants ramtest-sdr-pads --all-transitions --output-load-pf 30` fits the
39-pin fixture with one plain rising-edge DQ capture register instead of the
DDR input primitive, requesting `FAST_INPUT_REGISTER` on all DQ bits. This
matches FES's `RAM_OSS_HIGH_SPEED` capture workaround while preserving the
same registered data/OE and command/address/mask outputs.
`ramtest-sdr-pad-reference30.json` records all four corners and transitions:
rising capture setup6433ps and signed hold-2181ps, identical to the DDR high
word. The backend uses the same outward-rounded6440/-2180ps checks.

`ramtest-sdr-clock-reference.json` records90 registers and1080 audited clock
checks, with no opaque DDR input groups. Input ingress period1538ps and
high/low167/144ps fit within the backend's conservative1540/170/190ps input
envelope. `check_clock_summary.py` exercises six malformed-evidence cases
for this SDR reference and seven for DDR.

The native regression `../registered-pad/check.py --sdr` verifies SDR capture,
registered data/OE, setup rejection, clock-cut rejection and checkpoint replay.
The current FES high-speed QSF must request DQ input/output/OE packing before
using this pad profile; its older workaround leaves those registers in fabric.
This fixture does not modify FES RTL or certify a board timing budget.
