# Full FES RAM tester IO investigation

For the closure scope and Atari ST handoff, start with the
[issue #135 resolution](../../../docs/validation/mistral-sdram-issue-135.md).
The experiments below include historical and diagnostic RTL snapshots; the
production command/address IO-register fix already merged in FES #524.

The follow-up [placement reliability study](../../../docs/validation/mistral-sdram-placement-reliability.md)
records explicit HeAP settings, unsuccessful runs, the selected-artifact
hardware control and the remaining constraint coverage. Architecture defaults
are unchanged. To inventory saved SDRAM delay declarations, use:

```sh
python3 mistral/tests/ramtest-io/constraint_coverage.py \
  --checkpoint /tmp/ramtest/final.json --output /tmp/ramtest/coverage.json
```

A successful inventory means all active directions have declarations; it
does not establish timing or board acceptance.

For a qualified registered-pad checkpoint, also audit the actual channels:

```sh
python3 mistral/tests/ramtest-io/path_coverage.py \
  --checkpoint /tmp/ramtest/final.json --report /tmp/ramtest/timing.json \
  --output /tmp/ramtest/path-coverage.json
```

Generate the checkpoint and detailed report in the same invocation. The report
must include final analogue timing and per-endpoint `setup_checked`,
`hold_checked`, `setup_slack_ns` and `hold_slack_ns`. An unchecked slack is
`null`; an arrival can remain visible after a clock cut. The audit requires
every expected registered subchannel, the declared reference clock/edge and
the packed register clock/edge. DQ data and OE are separate channels; DDR
capture and forwarded clocks require both edges. Unsupported unregistered
pad directions fail this audit rather than receiving an implied pad model.

Coverage succeeds even when checked slacks are negative. Its separate
`all_channel_slacks_nonnegative` field does not certify the whole design,
board delays, waveform or turnaround. The compiler exit status and complete
timing summary remain mandatory. The [full-board path audit](../../../docs/validation/mistral-sdram-full-board-coverage.md)
records the retained 100/130 MHz diagnostic replay and its limitations.

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

These fits use a newer controller with an extra output register stage and
adjusted capture count, plus forced IO packing. They do **not** reproduce the
100 MHz hardware-passing OSS bitstream. The historical baseline audit below
supersedes interpreting their negative slack as a failure of that artifact.

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

## Calibrate against the historical 100 MHz hardware baseline

`baseline-reference.json` identifies the original artifacts from
[issue #135](https://github.com/DeanoC/nextpnr/issues/135): passing RBF
`e76deaa51662f65f…` (2180175 bytes), and failing RBF `9cff64b3a51fcf99…`
(2166676 bytes). Their controller, wrapper and PLL source hashes match each
other. The newer diagnostic wrapper/controller hashes do not match them:
the diagnostic adds an output stage and changes the 100 MHz capture count
from 2 to 3 to compensate. Both historical builds use fabric capture/output
registers; the diagnostic places capture and output registers in IO cells.

`legacy_checkpoint.py` repairs only isolated checkpoint metadata. The old
writer emitted duplicate `outclk` keys, so ordinary JSON loading discards a
PLL output. The helper identifies outputs from their routed C6/C7 source
counters, independently of key order, restores physical frozen-pin names
and the old clock periods/phases, then replays without pack/place/route.
Every output RBF must match the supplied original SHA-256 exactly. No source
worktree or original artifact is changed.

```sh
python3 mistral/tests/ramtest-io/legacy_checkpoint.py \
  --source-root /path/to/historical/sources/misteross \
  --nextpnr /path/to/nextpnr-mistral --expected-rbf ORIGINAL_FULL_SHA256 \
  --output /tmp/historical-replay --probe
```

Both exact artifacts replay successfully and preserve internal timing passes.
The original capture-to-controller setup margins are +3.371 ns passing and
+3.053 ns failing; the recent forced-IO diagnostic's negative margin is not
the historical passing implementation's margin. GPIO and PLL settings
decoded from the two original RBFs with one decoder match exactly (190 GPIO
and 64 PLL settings). Placement and routing still differ.

The optional probe retains top-level ports on packed reload and applies
synthetic zero input/output requirements to expose routes. Only the clock
forwarder receives the reference timing profile/load; no fabric register is
repacked. Probe RBFs also match the originals exactly. Synthetic probe timing
can fail, and that is not a hardware result or a SDRAM timing requirement.
Enabling IO analysis also uses full early/late clock-route intervals, so its
internal margins differ slightly from the original scalar clock-route checks.

The same native model reports read-path late arrival medians of 1.529 ns
passing and 1.852 ns failing. DQ OE late arrivals increase by 1.016 ns median;
the largest individual address-boundary increase is 2.491 ns. These are
differential observations at native GPIO/fabric boundaries, not complete
chip-pin delay budgets. Unregistered pad/package and fast-corner qualification
remain unresolved. They identify real route differences to investigate,
without proving which path caused the hardware failure.

Eight ideal-clock traces of the historical 100 MHz controller consume the
sample 10 ns after nominal chip data launch, with CAS2/burst1. Both fast and
maximum access assumptions succeed with zero assumed return delay; longer
return assumptions can fail. The model reproduces the consumed cycle but
does not establish physical board delays. The user's Quartus 100 MHz hardware
pass remains the baseline observation; its exact bitstream identity is still
pending, so the recent Quartus refits must not inherit that hardware result.

## Historical command and bus-turnaround audit

`legacy_sequence.py` runs the original controller and its original `mem_channel`
client without editing either source. Six patterns over four words at each of
five bases exercise low addresses, row A9/A12, column carry and chip-select
carry. The fixture observes actual output changes after nonblocking assignments
and command capture on the ideal forwarded clock. The board selects the second
chip through inverted chip-select, so both select polarities are included.

```sh
python3 mistral/tests/ramtest-io/legacy_sequence.py \
  --source-root /path/to/historical/sources/misteross \
  --replay-root /tmp/nextpnr-135-investigation \
  --output /tmp/historical-sequence
```

The replay root must contain `baseline-replay-passing` and
`baseline-replay-failing` from the exact-artifact replay above. Source hashes
reject newer diagnostic RTL; probe RBF hashes must match the historical
artifacts. `legacy-sequence-reference.json` records the resulting 480 commands.
Across these scans, every address/bank/command bit has a transition only 5 ns
before chip capture. Every write data bit has at least 35 ns between its last
change and capture. Thus treating all outputs as equally critical would hide
the actual controller schedule.

The largest address regressions map to these physical ports:

| Port | Pin | Passing native arrival | Failing native arrival | Increase |
| --- | --- | ---: | ---: | ---: |
| A9 | C12 | 6.306 ns | 8.797 ns | 2.491 ns |
| A10 | AB26 | 4.421 ns | 6.676 ns | 2.255 ns |
| A12 | D12 | 6.999 ns | 8.959 ns | 1.960 ns |

These arrivals include native launch-clock, fabric register and routing delay
to the internal GPIO input boundary. They omit unqualified unregistered pad
and board delays. For an address that changes 5 ns before ideal chip capture,
the setup condition is `arrival + pad/board + tIS <= 5 + chip-clock-delay`.
The reference records `arrival + 1.5 - 5` as a **lower bound on the required
chip-clock delay**, with missing pad/board delay set to zero. It is not a slack
verdict, and independently chosen extrema do not establish correlated timing.

The minimum ideal write-OE-release to next read-drive interval is 115 ns
(CAS2, burst1, `tLZ_min=0`). In the reverse direction, using `tHZ_max=5.4 ns`,
the interval from latest read release to next write-OE assertion is 59.6 ns.
The -6 timing parameters come from Table 16 of the
[Etron datasheet](https://etron.com/wp-content/uploads/2022/04/EM63B165TSBM_Rev-2.4.pdf).
Worst native OE boundary arrivals are 6.339 ns passing and 7.242 ns failing.
These route changes are small relative to the observed turnaround opportunities;
they do not support the earlier suspicion of a tight OE bus handoff in this
historical controller. This does not qualify electrical contention timing.

Address setup is the stronger lead from this audit. Input capture routing and
command skew remain possible causes. STA path maxima may not be sensitized by
every simulated transition, and neither this ideal-clock trace nor the route
comparison proves which path caused the hardware failure. The next useful
experiment should preserve the original RTL/capture schedule and change the
suspect placement/routing, with an identifiable artifact for hardware comparison.

## Controlled three-address-net hardware candidate

`legacy_reroute.py` starts from the exact failing historical checkpoint and
removes routing only for `sdram.sdram_a[9]`, `[10]` and `[12]`. The routed
checkpoint automatically freezes all loaded placements, physical pin maps
and remaining routes. It uses CPU router2 with `router2/estimateWeight=0`
to search beyond the usual heuristic choices, seed 2, without pack or place.
No RTL, IO packing or PLL/capture phase is changed.

```sh
python3 -B mistral/tests/ramtest-io/legacy_reroute.py \
  --baseline /tmp/nextpnr-135-investigation/baseline-replay-failing \
  --nextpnr /path/to/nextpnr-mistral --decoder /path/to/mistral-cv \
  --output /tmp/ramtest-100-address-control
```

The helper requires unchanged logic/connectivity, placement and pin maps;
exactly three changed physical routes; identical original internal final
analogue timing; 16 unchanged read boundaries and 49 unchanged other output
boundaries. It decodes both original and candidate RBFs with the same decoder
and requires every non-routing configuration line to match. Its synthetic
zero-delay probe must preserve the candidate RBF byte-for-byte; its timing
failure is a diagnostic result, not a hardware result. `--reuse` validates
existing completed runs rather than repeating routing/probing.

Three isolated CPU routing trials produced these native late boundary arrivals:

| Trial | A9 | A10 | A12 |
| --- | ---: | ---: | ---: |
| Original failing build | 8.797 ns | 6.676 ns | 8.959 ns |
| Router1 | 10.528 ns | 7.953 ns | 8.644 ns |
| Router2, normal search | 10.528 ns | 8.130 ns | 10.541 ns |
| Router2, estimate weight zero | 6.002 ns | 5.604 ns | 6.169 ns |

Only the final trial improves all three paths. An independent fresh run of
the helper reproduced the same compressed RBF:

- SHA-256: `fdf50532b84edc282dab9991469bc541ff5a6e5dfada569f8ed2b7c2e7e2fa72`
- Size: 2166657 bytes.
- File on powerboat: `/tmp/nextpnr-135-investigation/ramtest-100-address-control/candidate.rbf`.
- Receipt: `legacy-reroute-reference.json`.

All 19892 cells retain their physical locations and pin maps. All 51953 decoded
non-routing configuration lines match the original failing build, including
PLL and GPIO settings. The original internal setup margins remain +0.218 ns
on the memory clock and +3.053 ns on the capture clock. A9 improves 2.795 ns,
A10 1.072 ns and A12 2.790 ns relative to the failing artifact; A9 and A12 are
also faster than the known-passing artifact at these boundaries.

This is a controlled **hardware-test candidate**, with the completed hardware
comparison recorded below. It is not a board timing signoff. Native boundary
arrivals still exclude unqualified pad/board delays. Compare this RBF against
original failing SHA `9cff64b3a51fcf99…`
using the same kit, boot, clock rate and patterns. A pass would implicate the
three address routes as a group; a failure would leave read capture, command
skew and remaining address setup as possible causes.

## Local hardware comparison package bundle

`legacy_package.py` creates format-2 diagnostic `.fcore` envelopes for the
candidate and both exact historical controls. The controls retain their
original manifest and RBF bytes; the failing archive also matches the original
producer export byte-for-byte. The candidate retains the FPGA-embedded build
ID `26d08971ccc8f584488e1dddd1c53f13`, source revision, recipe and ABI because
none of that logic changed. Its prerelease version, name, payload digest,
package digest and appended post-route toolchain identity distinguish it.

This is local experimental packaging, not a fresh producer synthesis seal.
The original build identity is not regenerated or fabricated. The separate
reroute receipt documents the controlled post-processing; the standard
producer exporter still requires its own pinned source/build-input evidence.
Package checks do not confer hardware acceptance.

```sh
python3 -B mistral/tests/ramtest-io/legacy_package.py \
  --passing-root /path/to/original/passing/sources/misteross \
  --failing-root /path/to/original/failing/sources/misteross \
  --candidate /tmp/nextpnr-135-investigation/ramtest-100-address-control \
  --output /tmp/ramtest-comparison-packages
```

The historical strict Python package reader and FogCast's Go
`corepackage.InspectPackage` both accepted all three generated archives and
agreed on package/payload identities. Optional `--consumer-results` retains
the JSONL inspections only when they match those exact three packages.
The published local bundle is
`/tmp/nextpnr-135-investigation/hardware-comparison-package/ramtest-address-comparison.tar`.
It contains the three `.fcore` archives, checksum list, package/reroute receipts,
consumer inspection evidence, import instructions and an empty hardware-results
template. `legacy-package-reference.json` identifies the completed bundle.

Candidate package ID:
`1b941609aa4fe6e6695d4067b2447c0b468665a2f238901c2c4e88efa7e5ccaa`.
Passing control ID: `3593d7e08af70397580b48e4f8469069a643e8c42a88907ed8b2ce4ba4c17d1f`.
Failing control ID: `019484653963d99f455cd907157d293aa9fc833152f6717aa5e100270fe7b69f`.
The completed comparison below loaded the failing and candidate packages through
the leased target development-core API. No host library entries were created.

## Exact-package hardware comparison, 2026-10-06

The user authorized either kit. Kit B was idle with a free lease and accepted
the failing package, but its ASUS capture card was held by another process.
That attempt was stopped and its lease released without recording a test
outcome. Kit A had a free lease and an available ShadowCast capture card;
the complete comparison ran there without rebooting or changing its image.

| Same-boot run | Package | SDRAM patterns | SDRAM errors | ADDR / INVR errors | HPS DDR P0/P1/P2 |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | Original failing | 6/6 | 128 | 64 / 64 | PASS / PASS / PASS |
| 2 | Address-route control | 6/6 | 0 | 0 / 0 | PASS / PASS / PASS |
| 3 | Original failing again | 6/6 | 160 | 96 / 64 | PASS / PASS / PASS |

The four constant patterns had zero errors in all three runs. Both failing
runs reported first fault `007F3000`, last fault `00FF600F`, first observed word
`2F7F`. The total is not a fixed count across reloads, consistent with a
marginal physical path; the three-route candidate removed the observed errors.
These observations implicate A9/A10/A12 routing **as a group**, not one
individual address bit. They do not establish every board, frequency or PVT
corner, or replace the outstanding pad/board timing qualification.

Each package was checked by archive SHA-256 before transfer, and the target
returned the expected package ID, GP ABI and embedded build ID. The lease was
renewed throughout all three loads, captures and Stops. Each run was observed
for 200 seconds, with HDMI snapshots at 10/60/120/180/200 seconds. The 120-second
and final snapshots show the completed six-pattern sweep. The final Stop
restored idle, and the lease was released; the kit boot identity was unchanged.

`legacy_screen.py` decodes 1280×720 captures against the original RTL's 8×8
font at 3× scale. Header and fixed port labels establish pixel alignment;
every result glyph must match exactly, the six counts must sum to the total,
and SDRAM/HPS completion must be present. This avoids confusing the displayed
hexadecimal `80` with `800`. Original PNGs remain unmodified.

- [First failing capture](hardware-fail-first.png)
- [Passing address-route capture](hardware-address-pass.png)
- [Repeat failing capture](hardware-fail-repeat.png)

`hardware-reference.json` records target/image/boot identities, package and
payload digests, decoded observations, capture/command hashes and cleanup.
Raw API responses, all captures and the leased runner remain on powerboat in
`/tmp/nextpnr-135-investigation/hil-kita` (the incomplete Kit B attempt is in
`hil-kitb`). The next implementation step is to obtain these address timings
during normal constrained placement/routing; this experiment changed only
the three routes in a frozen historical checkpoint. The fresh build below
now exercises that step.

## Fresh constrained placement and routing

`normal_build.py` starts from the exact historical failing **synthesis** and
performs ordinary packing, placement and routing. It rejects physical
checkpoints, copies the original pin constraints, and changes no RTL or IO
register requests. No prior fabric placement or routes are imported.
`address-target.sdc` applies the same native arrival target to all 13 address bits: the 10 ns
clock period minus a 3.5 ns output maximum leaves 6.5 ns for clock routing,
clock-to-Q and the fabric-to-GPIO route. This is an empirical optimization
target derived from the hardware control, not a complete chip-pin budget.

```sh
python3 -B mistral/tests/ramtest-io/normal_build.py \
  --source-root /path/to/historical/failing/sources/misteross \
  --nextpnr /path/to/nextpnr-mistral \
  --output /tmp/ramtest-fresh-address --gpu-cpu
```

The default uses the GPU router, seed 2 and normal HeAP placement;
`--gpu-cpu` selects its CPU reference backend. Router2 is also selectable.
The output directory must be new. The helper retains the actual compiler
exit code and final analogue report; it does not suppress timing failures.
Input synthesis, constraints, command, runner source and tool identities are
retained with the result.

The complete CPU Router2 trial missed address and internal setup requirements.
The GPU router with analogue repair met all 13 address targets:

| Address | Fresh native late arrival | Margin against 6.5 ns target |
| --- | ---: | ---: |
| A9 | 5.292 ns | 1.208 ns |
| A10 | 5.815 ns | 0.685 ns |
| A12 | 4.878 ns | 1.622 ns |

A10 is the slowest of all 13 bits. The final setup WNS is +0.032 ns for
`ram_clock.clocks[0]`, +2.716 ns for its shifted capture clock, and +2.322 ns
for video. HPS hold WNS remains **−0.407 ns**, so the compiler exits **1** and
`complete_timing_pass` is false. The exact original failing artifact's IO probe
also reports HPS hold at −0.406 ns. The existing F2SDRAM output model has a
zero minimum delay; this remains conservative qualification work, and the
32 ps internal setup margin is small. A hardware pass does not convert this
report into complete timing signoff. Uncharacterized pad/package/board delays
also remain outside these native address targets.

Analogue repair now uses complete setup WNS rather than reconstructing margin
from Fmax. Tests cover a shifted-clock IO violation whose actual margin is
half the reconstructed value, and a physically related violation with no Fmax
entry. This same-edge fixture's constrained RBF is identical before and after
the repair correction. The timing-gate regression still rejects 400 MHz with
and without bitstream output and accepts its 10 MHz control.

Fresh RBF: `d1e5b33a42d99714f88365b579c594a01b0897cbc6031a133d0ef1ee99f7dc43`.
Experimental package: `53395a6681e78b45f469414600a5f01ea03fe349175ea2d868e2bfccc8df4c39`.
The strict historical Python reader and FogCast Go inspector agree on its
payload/package identities. Its embedded historical build ID is retained;
the separate fresh P&R receipt describes the new physical build. It is an
experimental envelope, not a fresh producer seal.

The complete Kit A same-boot comparison ran through the renewed target lease
and volatile development-core API, observing each load for 200 seconds:

| Run | Package | SDRAM sweep | SDRAM errors | ADDR / INVR | HPS DDR P0/P1/P2 |
| --- | --- | --- | ---: | ---: | --- |
| 1 | Fresh constrained build | 6/6 PASS | 0 | 0 / 0 | PASS / PASS / PASS |
| 2 | Original failing control | 6/6 FAIL | 228 | 116 / 112 | PASS / PASS / PASS |
| 3 | Fresh constrained build again | 6/6 PASS | 0 | 0 / 0 | PASS / PASS / PASS |

All constant patterns had zero errors. The failing control recorded first
fault `003F3800`, last fault `00FF700F`, first observed word `373F`;
the varying fault counts/addresses across reloads reinforce the need for
the failing control in each comparison. Both fresh-build 120-second and final
200-second captures show completed sweeps with zero errors. The target
attested the expected package, ABI and embedded build ID at every checkpoint.
Final Stop restored idle, the lease is free, and the target boot/image
identities match the start of the run.

- [Fresh-build first pass](normal-pass-first.png)
- [Original failing control](normal-failing-control.png)
- [Fresh-build repeat pass](normal-pass-repeat.png)

`normal-reference.json` retains the fresh-build receipt, rejected Router2
trial, package/consumer identities, observations, source/capture/command hashes,
cleanup and verification results. Across the affected IO-delay and timing-report
suites, 41 distinct tests have passing final outcomes; the timing-gate smoke
also passed. Raw build evidence is in
`/tmp/nextpnr-135-investigation/normal-address-qualified`, packages in
`hardware-normal-package`, and the leased runner/API/captures in
`hil-normal-kita` under the same investigation root on powerboat.

This establishes a passing fresh constrained build at 100 MHz on this kit.
It changes many physical paths; the earlier three-route control provides
the isolated evidence about A9/A10/A12. The production FES build recipe is
unchanged, and this experiment does not qualify 130 MHz or resolve the
remaining full timing checks described above.

### HPS hold diagnosis: declined route override corrupted fallback timing

The subsequent hold audit found a software error in
`Context::getNetinfoRouteDelayQuad`. Its physical-sink envelope was also passed
to the architecture override. A failed cached Mistral analogue arc writes a
default zero delay and returns false. The fallback then took the minimum of
zero and the real route-table minimum, retaining zero. This affected clock
routes as well as HPS data routes, so both setup and hold could be distorted.
The scalar route-delay API already used a separate override result.

The fix gives the override its own result and initializes the physical-sink
envelope only after an override is declined. It does not change the HPS
clock-to-output minimum, FF hold requirements, routing, RTL or constraints.
`CompletedObservationsTest.FailedCachedOverridePreservesFallbackRouteMinimum`
exercises a real unsupported HPS GIN hop: cached and uncached fallback delay
intervals must agree, retain their positive minimum, and agree with the scalar
API's maximum. Existing observation tests retain their completed-prefix checks.

Both replaying the hardware-tested `normal-address-qualified/final.json` and
a fresh normal build now exit **0**, without `--timing-allow-fail`. The fresh
build uses the original synthesis/QSF, seed 2 and the GPU router CPU reference
backend, with normal packing, placement and routing. All reported setup/hold
checks pass under the existing models and address targets. Use its final
margins as authoritative:

| Clock | Setup WNS | Hold WNS |
| --- | ---: | ---: |
| Memory 100 MHz | +0.032 ns | +0.418 ns |
| Shifted capture 100 MHz | +2.716 ns | +5.431 ns |
| Video 74.25 MHz | +2.322 ns | +0.410 ns |

The RBF SHA256 remains
`d1e5b33a42d99714f88365b579c594a01b0897cbc6031a133d0ef1ee99f7dc43`,
identical to the bitstream tested in the preceding 0 → 228 → 0 comparison.
The fresh build reproduces those same bytes and meets all 13 address targets
(maximum native arrival 5.815 ns). The earlier exit-1 receipt is retained as
historical evidence; the new timing results supersede it, not its hardware
observations. No additional hardware load was needed for an identical RBF.

The checkpoint replay reports +0.209 ns memory setup **both before and after**
the fix. This is not a setup improvement caused by the fix. The fresh build
retains analogue repair's routing calibration; checkpoint replay starts with
fresh calibration state. The fresh worst setup path starts at HPS
`cmd_ready_1`, whereas replay selects a fabric-launched path. Until calibrated
fallback replay is qualified, retain the smaller **32 ps** fresh-build margin.
Both flows report the corrected +0.418 ns hold margin.

The retained Quartus 17.0.2 OSS 100 MHz fit provides a separate HPS reference.
Run `quartus_sta -t /absolute/path/to/hps_hold.tcl /absolute/report-directory`
in the fitted project, then summarize with
`python3 hps_hold.py --reports /absolute/report-directory --output summary.json`.
The query preserves that project's SDC and checks all four operating corners,
limited to 2000 paths per case. HPS output hold WNS is +0.338/+0.430 ns in the
two slow corners and +0.213/+0.219 ns in the two fast corners. All examined
output hold CELL delays at the hard-block atom pins are **zero**. Thus an
invented positive HPS minimum would not match this Quartus reference. The
Quartus placement/routes differ from the historical OSS fixture; these
margins are comparison evidence, not transferable OSS slack.

All 45 IO-delay, timing-report and completed-observation unit tests pass.
The GPU timing-gate smoke continues to reject 400 MHz with and without RBF
output and accept 10 MHz. `hps-hold-reference.json` retains report hashes,
before/after replay results, fresh build identities and verification results.
Raw evidence is in `/tmp/nextpnr-135-investigation/hps-hold-audit` and
`/tmp/nextpnr-135-investigation/normal-address-hold-fixed` on powerboat.

Passing the existing timing gate does not qualify unmodelled board/pad paths,
ganged HPS clock arcs, all PVT corners or 130 MHz. The native address targets
remain optimization constraints, not complete SDRAM chip-pin budgets.

### Extra setup margin: bounded seed comparison and hardware repeat

`normal_build.py` now accepts `--setup-margin-ps`, `--analogue-rounds` and
`--timeout-seconds`. The requested setup margin is a repair target, separate
from the timing gate. Receipts record both `complete_timing_pass` and
`setup_margin_target_met`; reaching zero slack does not imply that a positive
requested target was achieved. Positive targets also raise `analogueRipSlack`
to at least that target, so routes between zero and the target are eligible
for repair. Defaults preserve the original zero-margin, 1200-second recipe.

For this study, the original synthesis/QSF and address SDC were unchanged.
Three fresh builds used the GPU router CPU reference backend, seeds 1/2/3,
a **500 ps** setup target and at most **five** analogue reroute rounds:

```sh
python3 mistral/tests/ramtest-io/normal_build.py \
  --source-root /path/to/original/failing/misteross \
  --nextpnr /path/to/nextpnr-mistral \
  --output /new/output/directory \
  --gpu-cpu --seed 2 --setup-margin-ps 500 --analogue-rounds 5
```

| Seed | Result | Final memory setup WNS | Final memory hold WNS | Address targets |
| --- | --- | ---: | ---: | --- |
| 1 | Timeout at 1200 seconds; no final result | unavailable | unavailable | unqualified |
| 2 | Compiler exit 0; full timing gate passes | +0.435 ns | +0.418 ns | all 13 met |
| 3 | Compiler exit 1; internal setup fails | −0.862 ns | +0.413 ns | all 13 met |

None established the requested 500 ps margin. Seed 1's last intermediate
setup margin was −0.652 ns; it is not a final signoff result. Seed 3's worst
internal path launches at `ddr1_test.idle_MISTRAL_FF_Q_23` and ends at
`ddr1_test.beats_left_MISTRAL_FF_Q_7.ENA`. This isolates a remaining internal
placement/routing issue even when the address targets pass. Do not infer
timing closure across seeds from the selected seed-2 result.

Seed 2 improves the fresh setup margin from **32 ps to 435 ps**, with the
same 20104 cells, logic and BEL placements as the zero-margin seed-2 build.
Connections were compared by complete net-name/bit alias sets because JSON
export bit IDs differ. Extra repair changed **455 routes**, including
`dq_out[2]` and `dq_oe`. Address and fabric read-capture routes remain unchanged.
The maximum native address arrival remains 5.815 ns. Capture setup/hold remains
+2.716/+5.431 ns; video setup/hold remains +2.322/+0.410 ns.

Selected RBF:
`a9ec99435c4a858443ca320b278fcdcb84d530756df99f95dbf0d642dcbec421`.
Experimental package:
`08796300446c7b94aaf77a8b5fcefd6279007e5c9f2c5600eee80d2df2ea2fba`.
The package preserves the original embedded build ID and is a post-route
experimental envelope, not a new producer seal. Strict historical Python
and FogCast Go package readers agree on its identities.

The selected candidate was tested on Kit A through the renewed lease and
volatile development-core API, for 200 seconds per load on the same boot:

| Run | Package | SDRAM patterns | Errors | ADDR / INVR | HPS DDR P0/P1/P2 |
| --- | --- | --- | ---: | ---: | --- |
| 1 | 435 ps candidate | 6/6 PASS | 0 | 0 / 0 | PASS / PASS / PASS |
| 2 | Original failing control | 6/6 FAIL | 480 | 264 / 216 | PASS / PASS / PASS |
| 3 | 435 ps candidate again | 6/6 PASS | 0 | 0 / 0 | PASS / PASS / PASS |

Both 120-second and final 200-second captures show these completed results.
The control's first/last fault addresses were `003F7000`/`02FF2003`; all constant
patterns and HPS checks had zero errors. Package, build ID, ABI and volatile
mode were attested at each checkpoint. Final Stop/release left the target
**idle/free**, with boot, runtime and image health identical to the start.

- [Candidate first pass](margin-pass-first.png)
- [Original failing control](margin-failing-control.png)
- [Candidate repeat pass](margin-pass-repeat.png)

The seed-1 timeout exposed a missing diagnostic receipt. Future timeouts now
write `timeout-receipt.json`, return 124 and explicitly report no final timing
result. An actual compiler run with `--timeout-seconds 1` verifies the receipt,
input/log hashes and absence of an RBF. The study's original seed-1 run used
the earlier helper; its raw timeout and last intermediate result are retained
separately, without manufacturing a completed build receipt.

`margin-reference.json` retains every result, original per-build receipts,
the normalized routing comparison, package identities, decoded observations,
API/capture hashes and cleanup. Raw evidence is under
`/tmp/nextpnr-135-investigation/margin500-seed{1,2,3}`,
`hardware-margin500-package`, `hil-margin500-kita` and `margin-timeout-smoke`
on powerboat. Only the diagnostic helper changed in this study; the compiler
binary was the previously verified HPS timing fix.

This establishes a hardware-passing 100 MHz candidate with more setup margin.
The 500 ps target, closure across seeds, complete SDRAM board/PVT timing,
integration of these new experimental constraints into the production FES
recipe, and 130 MHz remain unfinished. The earlier production IO-register
fix is already merged; it is distinct from these historical constrained builds.
