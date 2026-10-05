# Full FES RAM tester IO investigation

Use an isolated snapshot of FES; do not modify another task's worktree. The
current investigation uses FES `8f5c5b5982fea01877b223a36cca0024dd15a8c8`, the
`build_fes_ramtest.py` synthesis recipe for `RAM_OSS_HIGH_SPEED`, and both
100/130 MHz variants. This is a diagnostic build with a fixed BUILD_ID, not a
sealed FES package. CPU router2 results are not the authenticated GPU lane's QoR.

Append these QSF assignments to the snapshot:

```tcl
set_instance_assignment -name FAST_INPUT_REGISTER ON -to SDRAM_DQ[*]
set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to SDRAM_DQ[*]
set_instance_assignment -name FAST_OUTPUT_ENABLE_REGISTER ON -to SDRAM_DQ[*]
```

Enable `NEXTPNR_GPIO_TIMING_PROFILE QUARTUS_17_0_2_RAMTEST` and
`BOARD_MODEL_FAR_C 30P` on DQ, A, BA, CKE, nCS, nRAS, nCAS, nWE and CLK.
The 30 pF load is a declared reference-model envelope, not a board measurement.
DQML/DQMH are constant outputs in this recipe; do not claim registered mask
timing from this experiment. Check the packed design for all 16 DQ input,
output and OE registers, the 20 address/bank/command outputs, and the AD20
constant `DDR_HIGH=0` clock forwarder.

First route with the ordinary 50 MHz reference-clock SDC plus a diagnostic
clock-pad constraint to expose both complete forwarded-clock arrival intervals:

```tcl
set_output_delay -clock {ram_clock.clocks[0]} -min 0 [get_ports SDRAM_CLK]
set_output_delay -clock {ram_clock.clocks[0]} -max 0 [get_ports SDRAM_CLK]
```

Use `--detailed-timing-report`, `--write`, `--report` and `--rbf` to retain
the routed checkpoint and final analogue timing report. Allowing timing failure
is only for diagnosis. No artifact from an allowed-failure build establishes
hardware acceptance. Multiple-output PLL checkpoint replay must retain every
output and counter; `../pll/checkpoint.py` verifies this with a three-output PLL.

`budget.py` reads that report and checkpoint. It requires explicit board delay
intervals and margin; it provides no measured-board defaults. For example,
the following **illustrative assumptions** use 0..0.5 ns flights, 0..5 ns for
the second chip's CS inverter, and 0.2 ns additional margin:

```sh
python3 mistral/tests/ramtest-io/budget.py \
  --report /tmp/ramtest/clock-probe-final.json \
  --checkpoint /tmp/ramtest/probed-final.json --output /tmp/ramtest/budget \
  --memory-mhz 100 --clock-flight 0 0.5 --data-flight 0 0.5 \
  --read-flight 0 0.5 --cs-inverter 0 5 --margin 0.2
```

The [Etron EM63B165 Rev2.4 specification](https://etron.com/wp-content/uploads/2022/04/EM63B165TSBM_Rev-2.4.pdf),
Table11, supplies -6 timing: CL2 tAC=6 ns at 100 MHz; CL3 tAC=5.4 ns at
130 MHz; tOH=2.5 ns, tIS=1.5 ns and tIH=0.8 ns. For clock arrival C,
outgoing data flight D and incoming read flight R, the budgets are:

```
input maximum  = Cmax + tAC + Rmax + margin
input minimum  = Cmin + tOH + Rmin - margin
output maximum = tIS + Dmax - Cmin + margin
output minimum = Dmin - Cmax - tIH - margin
```

C already includes the actual clock route and the complete mux/pad model,
plus the declared clock flight. Do not add FPGA pad latency again. Reference
these budgets to the **falling** edge of `ram_clock.clocks[0]`, because that
fabric edge produces the chip's rising edge. The capture clock remains the
PLL-derived phase-related `ram_clock.clocks[1]`; do not cut their relationship.
For nCS, use the stricter bound of the direct and inverted chip paths.

The script also evaluates chip high/low pulse lower bounds against 2 ns using
independent early/late envelopes. Negative lower bounds mean the model cannot
prove the waveform, not that the physical waveform is negative. Zero early
bounds and uncorrelated corners may make both rates fail despite a working
Quartus configuration. No observed minimum is substituted to force a pass.
Clock jitter, load correctness, actual board delays, controller command spacing,
and read/write turnaround (tLZ/tHZ and OE sequencing) still require validation.

Apply `board.sdc` to a checkpoint that preserved the IO clock constraints for
an initial fixed-route diagnostic. Keep the clock-pad probe constraint when
rerouting with the board constraints; re-extract clock arrival afterwards,
since changed routing can invalidate the previous budget. Do not declare
closure from a stale clock-route budget or from internal Fmax alone.

## Retained diagnostic result

`reference-results.json` records source/tool/artifact hashes, packing checks,
the explicit assumptions above and both final analogue reports. There are
70 registered-pad endpoint checks per rate: 16 input captures, 32 DQ data/OE
outputs, 20 address/bank/command outputs and two forwarded-clock edges.

| Rate | Worst setup slack | Worst hold slack | Chip high/low lower bounds |
| --- | --- | --- | --- |
| 100 MHz | -9.206 ns | -2.189 ns | -1.246 / -1.266 ns |
| 130 MHz | -15.914 ns | -3.343 ns | -2.400 / -2.420 ns |

The corrected input model's fixed-route replay is retained in
`input-zero-results.json`. Bitstreams are byte-identical to the original
routes. Worst setup becomes -7.578 ns at 100 MHz and -11.384 ns at 130 MHz;
worst hold remains -2.189/-3.343 ns. The100 MHz worst setup is now nCS output,
while130 MHz still has a read-capture violation. These results use the same
unmeasured board assumptions. The table above preserves the original model's
results; pulse bounds and internal fabric failures are unchanged.

Both diagnostic designs fail. These are fixed-route checks with assumed board
delays, not a conclusion that the board cannot run at these rates. The clock
envelope is too pessimistic to prove pulse widths; internal fabric timing also
fails independently. Further work needs corner-correlated clock/data bounds
and placement/routing improvement, before selecting an SDRAM capture phase.

Strict replay without `timing/allowFail` reports timing errors and returns a
nonzero status. The existing backend still writes an RBF after nonfatal timing
errors, so file existence is not acceptance: consumers must check exit status.
No bitstream from this investigation was programmed onto hardware.

## Corner correlation diagnostic

`corner_windows.py` audits the retained four-corner SDR reference, including
all 16 DQ pins, explicit pad transitions and the H=0/L=1 forwarding transfers.
It keeps early/late clock prefixes and setup/hold checks in the same corner.
It includes fitted upstream clock paths separately from the local mux/pad
arcs; these routes belong to the surrogate fixture, not the FES PLL design.

```sh
python3 mistral/tests/ramtest-io/corner_windows.py \
  /tmp/nextpnr-135-investigation/gpio-characterization/ramtest-sdr30/evidence.json \
  --flight-max 0.5 --margin 0.2 --clock-distortion 0.1 \
  --check-rejections --output /tmp/corner-windows.json
```

Flight is an assumed 0..0.5 ns interval for clock and return data. Clock
distortion is a separate assumed 0.1 ns maximum difference between rising
and falling board propagation; common trace flight does not shorten a pulse.
The retained `corner-reference.json` includes input/audit hashes and per-pin
windows. With these assumptions, minimum fitted reference pulses are
3.455 ns at 100 MHz and 2.301 ns at 130 MHz. The conservative native envelope's
negative lower bounds therefore do not establish a physical pulse failure.

Read capture still has no common absolute window across all corners under
these assumptions: the intersected lower/upper limits are 24.451/22.616 ns
at 100 MHz and 22.697/19.154 ns at 130 MHz. A lower limit beyond the upper
limit means the intersection is empty. This preserves the same read latency
across corners; wrapping individual windows modulo the period could silently
accept different words. It does not prescribe a new PLL phase.

The audit rejects missing corners, clock transfers and DQ pins, unsupported
device/load, and nonfinite timing. A shared 1 ns clock-prefix translation must
cancel from both pulse bounds and read windows. These checks do not qualify
the reference envelopes for production. These historical surrogate windows use nonzero input selectors. They do not
represent the corrected native zero-selector profile below.

## Input profile qualification mismatch

Isolated Quartus 17.0.2 fits of the actual OSS high-speed RTL at 100/130 MHz
are retained in `full-quartus-reference.json`. They request the same three DQ
register types and 30 pF load. Memory and capture clocks remain related; IO
delays are zero only to expose paths. These builds are different from the
user's passing Quartus DDR-capture core and establish no hardware acceptance.

Both full fits give SDR input setup 396..1905 ps and signed hold
-1683..-316 ps. Output late bounds reproduce the surrogate fit's
5276 ps data / 5416 ps OE / 4876 and 5372 ps selected clock transfers.

Decoded bitstreams identify a material qualification mismatch: all 16 DQ
pins in the surrogate use `RB_T1_SEL_IREG_CFF_DELAY=10` and
`SET_T3_FOR_CDATA0IN/1IN=7`. Those assignments are omitted (database defaults)
in the full Quartus fit and native tester. All 16 full/native input delay
settings agree. Merely checking that QSF delay attributes were absent did
not ensure the fitter selected the native configuration.

The original 6440/-2180 ps profile was not qualified for the native defaults.
Its -2180 ps hold requirement was less restrictive than the full fit's -316 ps
maximum. Controlled SDR/DDR fits now force D1/D3=0, and decoded settings
match the native defaults on every DQ pin. The new receipt
`../gpio-timing/input-zero-reference30.json` verifies the same 1905/-316 ps SDR
and DDR-high extrema, plus 1900/-306 ps DDR-low extrema. The backend rounds
outward to 1910/-310 ps and 1900/-300 ps. The separately audited input-to-fabric
maximum remains 866 ps. Historical receipts preserve the original observations;
use the corrected profile for subsequent native timing diagnosis.

Exploratory full-fit corner arithmetic retains PLL compensation through
accumulated clock arrival times, rather than summing data IC increments.
It still mixes independent early/late common-clock prefixes conservatively;
it neither credits common-path pessimism removal nor prescribes a capture
phase. These diagnostic windows must not be substituted for native STA.

## Output transfers in matching fitted corners

`corner_windows.py` now audits the complete set of 52 registered FES output
channels (16 DQ data, 16 DQ OE and 20 address/bank/command channels). The
surrogate's two registered mask outputs are optional as a complete pair.
It compares each channel against the same corner's physical clock transfer,
retaining independent early/late common-clock paths without CPPR credit:

```
write setup = T/2 + clock_early - output_late - tIS - flight_max - margin
write hold  = T/2 + output_early - clock_late - tIH - flight_max - margin
```

`transfer-reference.json` uses each full Quartus fit only at its actual rate,
with the earlier 0..0.5 ns flight and 0.2 ns margin assumptions. Before adding
the chip-select inverter, the worst output setup margins are -0.220 ns at
100 MHz and -1.342 ns at 130 MHz, both on DQ OE. Worst output hold margins are
+0.246 ns and -0.908 ns. The minimum nCS setup allowance for an additional
inverter is +0.710 ns at 100 MHz and -0.444 ns at 130 MHz. These numbers do
not establish the actual inverter delay, board flights, or native-route slack.
They explain why replacing the arbitrary 5 ns inverter allowance alone would
not establish complete timing closure. The audit rejects missing output
channels and requires shared-clock translation to cancel from output margins
as well as pulse bounds and read windows.

For a full PLL fit, pass `--memory-mhz 100` or `--memory-mhz 130` to retain
only its actual rate. The surrogate fixture may still explore both periods.

## Controller cycle sensitivity after IO packing

`capture_cycle.py` instantiates the snapshot's actual `sdram_addon_port.v`
with `RAM_OSS_HIGH_SPEED` and one output-register stage. Its diagnostic
wrapper reproduces the 5 ns / 6.538 ns rising capture clocks, plus the 130 MHz
falling-edge handoff in `top.v`. PLL and wrapper hashes are retained; changed
capture assignments or phase parameters require re-auditing the wrapper.
The existing FES simulation PLL passes the board clock through, and its
memory model supports only CAS2, so those models cannot establish this
100/130 MHz capture sequence.

```sh
python3 mistral/tests/ramtest-io/capture_cycle.py \
  --fes-root /tmp/fes-snapshot/sources/misteross \
  --output /tmp/capture-cycle-audit
```

The model launches one word CAS2/CAS3 chip clocks after READ, drives a poison
word before data becomes valid and after the following chip clock plus
2.5 ns hold, and records the exact capture consumed by the controller. Its
return delay is a lumped illustrative parameter; it is not a fitted FPGA arc
or measured board flight. The 32 traces sweep 0/3/6/9 ns return delays and
fast/maximum chip access assumptions. No RTL in FES is edited. Temporary
copies explore advancing the 130 MHz `capture_due` trigger by one or two
cycles; they are diagnostics, not proposed production patches.

`capture-cycle-reference.json` shows the original controller consuming a
sample 10 ns after chip data launch at 100 MHz, and 18.075 ns after launch at
130 MHz. With a fast return, 100 MHz consumes the valid word while 130 MHz
consumes poison. Advancing the 130 MHz trigger two cycles selects the sample
2.691 ns after launch and restores the word in that fast case. A delayed
return reverses the outcome: the original schedule can then succeed while
the advanced schedule fails. Every consumed word is checked against the
recorded sample time and explicit valid window.

This exposes a capture-cycle dependency that moving DQ registers into IO
cells can change. It does not prove that a fixed counter change repairs the
physical board, nor explain the original 100 MHz failure by itself. A final
capture schedule must agree with fitted clock/data timing, single-word read
latency, hold/turnaround requirements and hardware results. Do not add a
setup-only multicycle merely to make the late-read path pass: the selected
word's following transition must also be checked for hold.

## Standard-board specifications and candidate gate

The pinned [XSDS3.0 schematic](https://github.com/MiSTer-devel/Hardware_MiSTer/blob/bbd3619620056a0f44476e27f18b442b4f0a5952/releases/sdram_xsds_3.0.pdf)
shares CLK, address, commands and DQ between two memories, and uses an
LVC1G04 for the second chip select. It does not identify the inverter vendor
or establish parasitics for the user's RetroRemake board and DE10 connector.

[Etron Table 14](https://etron.com/wp-content/uploads/2022/04/EM63B165TSBM_Rev-2.4.pdf)
specifies input capacitance up to 5.5 pF and DQ capacitance up to 6 pF at 25 °C;
these are sampled specifications. Two chips therefore contribute 11/12 pF
before the PCB and connector. An illustrative extra 8 pF gives 19/20 pF.
This is a sensitivity assumption, not a measured 20 pF board limit. The native
profile retains its 30 pF timing envelope; lowering a declared load alone
cannot narrow its fitted timing bounds. FPGA input capacitance and the other
memory's DQ loading also matter to the read path.

[TI's SN74LVC1G04](https://www.ti.com/lit/ds/symlink/sn74lvc1g04.pdf)
at 3.3 V specifies maximum propagation delay of 3.3 ns at 15 pF, or 4.2 ns at 50 pF.
The comparison requires that exact part, its supply/temperature conditions
and input transitions no slower than 2.5 ns. A generic LVC1G04 marking does not
justify assigning these values to the user's board. The original 5 ns bound
remains the generic diagnostic assumption.

`board_gate.py` compares these assumptions against the retained full-fit
100 MHz evidence. It keeps one absolute read window across corners and does
not choose a PLL phase when the intersection is empty. TI 15 pF timing is
ineligible when the assumed load exceeds 15 pF; no load extrapolation is used.

```sh
python3 mistral/tests/ramtest-io/board_gate.py \
  --reference mistral/tests/ramtest-io/transfer-reference.json \
  --extra-cap-pf 8 --output /tmp/board-reference.json
```

`board-reference.json` also retains a strict native 100 MHz replay with the
conditional 0.7–3.3 ns inverter assumption. Worst setup improves from −7.578 ns
to −5.878 ns; worst hold remains −2.189 ns. It returns exit 1, and its diagnostic
RBF is identical to the original routed bitstream. No passing candidate was
selected or programmed: the read-window intersection is still empty and
output/internal timing still fails under the retained envelopes.

Etron's AC timing uses a 30 pF test load and 1 ns input transitions. Notes 9/10
require compensation for slower edges. Those conditions are not proven by
this investigation's pulse-width check or scalar pad-delay envelopes.
Further work must audit native clock/data correlation and waveform/load
conditions before pairing a physical capture phase with a consumption cycle;
tightening only the inverter assumption is insufficient.

## Compare full OSS RTL in Quartus and nextpnr

`quartus_board.tcl` applies the same explicit flight and external-margin
assumptions to retained full Quartus fits, using a generated clock at the
physical SDRAM pin. It queries all four corners without refitting the design.
Run from the full OSS-RTL Quartus project directory:

```sh
quartus_sta -t /path/to/quartus_board.tcl 100 /tmp/quartus-board100 3.3
python3 /path/to/quartus_compare.py \
  --reports /tmp/quartus-board100 --pad-evidence /path/to/evidence.json \
  --memory-mhz 100 --inverter-max-ns 3.3 --output /tmp/comparison100.json
```

The inverter argument records the conditional TI comparison; it does not
identify the installed part. Use 130 and its corresponding fit/evidence to
repeat the higher-rate audit. Quartus retains its derived PLL uncertainty
in addition to the external margin; native uncertainty is not modeled here.

The audit verifies the generated waveform and every external path's physical
clock chain: falling fabric mux ingress, falling-to-rising DDIO transfer,
then rising SDRAM_CLK at PIN_AD20. It requires all 16 inputs, 52 data/OE/control
outputs and 16 controller handoffs, verifies slack arithmetic and retains
each path's explicit clock-pessimism correction. Earlier exploratory reports
were not acceptance evidence until this edge mapping was checked.

`quartus-comparison-reference.json` records these results:

| Full Quartus fit | 100 MHz | 130 MHz |
| --- | ---: | ---: |
| Read setup slack | −5.161 ns | −11.859 ns |
| Read hold slack | +4.663 ns | +9.663 ns |
| Output setup slack, conditional inverter | −0.733 ns | −1.887 ns |
| Output hold slack | +2.054 ns | +0.900 ns |
| Capture-to-controller setup slack | −1.820 ns | −2.056 ns |
| Common capture-event shift interval | [5.161, 4.663] ns | [11.859, 9.663] ns |

The shift intervals compare moving the existing capture event later against
its setup and hold slacks on fixed routes. The 100 MHz intersection misses
by 0.498 ns; the 130 MHz intersection misses by 2.196 ns. These intervals are
not PLL settings: changing phase can select another edge/cycle and must be
checked against the controller's consumed word. At 100 MHz the controller
handoff is the IO capture to `sdram.rdata`; at 130 MHz it first reaches the
falling-edge `dq_oss_hold` register. Both have independent Quartus violations.
Without the report's shared-clock correction, these same intersections miss
by 2.492 ns and 4.190 ns respectively. This measures the correction's effect
in the Quartus fit; it does not transfer that credit to native clock routes.

The slowest *local* output arcs closely agree: Quartus reports 5276 ps data,
5416 ps OE and 5372 ps fabric-fall-to-pad-rise, versus native bounds of
5280/5420/5380 ps. The native minima are zero, while the full-fit observations
start at 2472/2545/2906 ps respectively. Native bounds also combine corner
envelopes and omit TimeQuest's common-clock correction. These differences
explain why total native slack cannot be interpreted as a direct measure of
how far its physical routes lag Quartus. Observed minima are still not
guaranteed silicon minima and do not authorize relaxing production bounds.

These are full Quartus fits of the actual OSS high-speed SDR-capture RTL.
They are separate from the previously reported working Quartus DDR-capture
core. Neither the current native results nor these Quartus results establish
hardware closure for this configuration. The remaining work needs a qualified
clock/pad model and a capture schedule that also meets the controller handoff;
moving a PLL phase alone is insufficient.
