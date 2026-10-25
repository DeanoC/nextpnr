# LAB FF asynchronous-clear timing reference

These Quartus 17.0.2 Build 602 fits characterize local `dffeas` clear
recovery/removal on `5CSEBA6U23I7`. They are bounded reference envelopes,
not silicon, native routed timing, reset-pulse or MTBF qualification.

```sh
python3 mistral/tests/ff-reset/characterize.py \
  --quartus-bin /path/to/quartus/bin --output /tmp/ff-reset-reference
python3 mistral/tests/ff-reset/test_reference.py
python3 mistral/tests/ff-reset/check_cli.py /path/to/nextpnr-mistral
```

The `rates` fixture checks 100, 50 and nominal 74.25 MHz rising clocks with
constant and dynamic enables. `slots` fixes every FF0..3 position in all ten
ALMs of two LABs, with constant or dynamic enables. `slots-ce` adds a third
LAB using inverted dynamic enables. Distinct virtual data inputs preserve the
80/120 sink registers. The parser requires every expected endpoint at every
corner and verifies the fixed locations, clear/clock pins and clock identities.
It rejects incomplete, duplicate, wrongly placed or wrongly clocked reports.
Source identity permits only Quartus's known version bookkeeping and blank
QSF lines to change. Fresh fits never overwrite existing output directories.

The retained 1,648 observations cover all four available slow/fast temperature
corners. Local recovery is the clear CELL delay minus the internal clock CELL
delay plus intrinsic recovery. Removal reverses the CELL subtraction and adds
intrinsic removal. Fabric routing and clock uncertainty are separate.
Observed maxima are 164 ps recovery and 43 ps removal. The opt-in backend uses
170 ps recovery, rounded outward to 10 ps, and retains the historical 270 ps
hold floor. Thus it tightens recovery without weakening an existing hold check.
The characterizer rejects a rebuilt reference that exceeds those model limits.

The guarded mode requires the tested device, a driven noninverted active-low
clear, exact rising clock waveform with period at least 10 ns and high/low at
least 5 ns, constant-active or driven dynamic enable, and inactive SCLR/SLOAD.
It excludes inverted clocks/clears and unsupported overrides. It does not
characterize registered GPIO clears, reset pulse widths, PLL lock/relock,
fast-corner routing, all possible input slews/placements, or metastability.
Unrelated/asynchronous sources remain unpaired. No timing exception is added.

`reference-summary.json` lists the bounds and hashes; `reference.json.gz` retains
the complete observations, source and raw-report hashes. The command above
rebuilds the fits and reports, and publishes output only after all timing and
source checks pass. A successful local reference still does not establish
physical-board acceptance.

The recovery/removal queries explicitly select rising transitions from the
fixture clear sources. CLRN is active low; an unrestricted removal query can
select its falling assertion edge instead. The parser independently requires
a rising transition at every sink's `clrn` pin and checks the report's
`From Node` against the fixture RTL. The checked-in archive was regenerated
with Quartus Prime Lite 17.0.2 Build 602 using those queries. All 1,648
observations record their validated `clear_source`, and all fitted checks pass.
No timing limits or backend behavior changed. This bounded fitted reference
does not qualify native fast-corner routing, reset pulses, MTBF or a board.

Calibration queries for disconnected clears with the explicit packed inactive
`PIN_1` state retain the ordinary data approximation; no reset path exists on
those ports. Missing or undriven signal clears remain unsupported.
