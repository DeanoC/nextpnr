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

Both diagnostic designs fail. These are fixed-route checks with assumed board
delays, not a conclusion that the board cannot run at these rates. The clock
envelope is too pessimistic to prove pulse widths; internal fabric timing also
fails independently. Further work needs corner-correlated clock/data bounds
and placement/routing improvement, before selecting an SDRAM capture phase.

Strict replay without `timing/allowFail` reports timing errors and returns a
nonzero status. The existing backend still writes an RBF after nonfatal timing
errors, so file existence is not acceptance: consumers must check exit status.
No bitstream from this investigation was programmed onto hardware.
