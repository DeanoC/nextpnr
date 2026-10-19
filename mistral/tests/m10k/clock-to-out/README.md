# M10K read clock-to-output (#232)

nextpnr used to give every M10K read a flat 1004 ps clock-to-output.
TimeQuest (Quartus Prime Lite 17.0.2, 5CSEBA6U23I7, Slow 1100 mV 100 °C)
times an unregistered read at 2.55–2.70 ns from the RAM's clock pin, so every
M10K-launched path read about 1.6 ns optimistic. `delay.cc` now takes each
read's clock-to-output from the class tables below. These are Quartus timing
models, not silicon measurements.

## Classes

A class is the read port's mode and whether the read goes through the M10K
output register. `Arch::m10k_output_registered()` decides that the same way the
bitstream programs `A/B_OUTPUT_SEL`: `CFG_OUT_REG_A/B` (both halves of a 40-bit
SDP word), or an active output clear (`ACLR1` for the B half, `ACLR0` for the A
half in TDP and 40-bit SDP modes). The B1DATA[19:0] bits of a 40-bit SDP read
come from the A half.

Setup is the worst arc of TimeQuest's setup analysis. Hold is the best arc of
its hold analysis, both at Slow 1100 mV 100 °C, over every width, bit, edge
and fit below.

| Class | TimeQuest setup | TimeQuest hold | `delay.cc` min / max (ps) |
| --- | ---: | ---: | ---: |
| SDP, unregistered | 2.592–2.620 ns | 1.441–1.454 ns | 1004 / 2620 |
| SDP, either port 40 bits, unregistered | 2.594–2.704 ns | 2.246–2.339 ns | 1004 / 2704 |
| TDP port A, unregistered | 2.578–2.610 ns | 1.422–1.440 ns | 1004 / 2610 |
| TDP port B, unregistered | 2.548–2.581 ns | 1.403–1.422 ns | 1004 / 2581 |
| SDP, registered | 0.667–0.709 ns | 0.566–0.603 ns | 566 / 709 |
| SDP, either port 40 bits, registered | 0.674–0.749 ns | 0.574–0.641 ns | 574 / 749 |
| TDP port A, registered | 0.697–0.744 ns | 0.592–0.634 ns | 592 / 744 |
| TDP port B, registered | 0.667–0.709 ns | 0.569–0.603 ns | 569 / 709 |

An unregistered read keeps the old 1004 ps minimum rather than TimeQuest's
slow-corner hold value. nextpnr checks hold at this corner only, and at
TimeQuest's fast corners the same reads hold for just 0.72–1.17 ns, so no hold
check becomes looser than before. `classes.json` has all four corners.

TimeQuest times an unregistered read from the port's read register
(`PORT_B_WRITE_ENABLE_REG` in SDP, `port[ab]_we_reg` in TDP): a 1.550–1.598 ns
cell from the clock pin, then 0.994–1.054 ns through the array to `dataout`.
In a 40-bit mode the split is 2.502 ns and 0.092–0.202 ns. A registered read
starts at the output register: 0.642–0.663 ns from the clock pin and
0.025–0.091 ns to `dataout`. `utco` is zero throughout.

## Write side (unchanged)

The same fits time the write and read-address inputs (setup = data pin cell −
clock pin cell, hold = the reverse). `delay.cc` keeps its existing values for
these; they are listed for comparison only.

| TimeQuest pin | nextpnr port | TimeQuest setup | `delay.cc` setup | TimeQuest hold | `delay.cc` hold |
| --- | --- | ---: | ---: | ---: | ---: |
| `porta/baddr` | `A1ADDR`, `B1ADDR` | −0.115–0.190 ns | 125 ps | 0.341–0.478 ns | 42 ps |
| `porta/bdatain` | `A1DATA`, `B1DATA` (TDP) | −0.187–0.097 ns | 97 ps | 0.441–0.562 ns | 42 ps |
| `porta/bwe` | `A1EN` (SDP), `A1WE`, `B1WE` | 0.197–0.352 ns | 140 ps | 0.462–0.627 ns | 42 ps |
| `portabyteenamasks` | `A1BE` | 0.070–0.085 ns | 140 ps | 0.439–0.457 ns | 42 ps |
| `ena0`, `ena1` | `B1EN` | 0.117–0.118 ns | 161 ps | 0.112 ns | 42 ps |

## Fits

`project.py` writes one Quartus project with 37 M10Ks: simple dual-port at
every width (1, 2, 5, 10, 20, 40), registered and unregistered, one or two
clocks, byte-enabled, mixed widths (10/20, 10/40, 20/40 both ways), true
dual-port at 1, 2, 5, 10 and 20 bits with each port registered or not, mixed
10/20 TDP, one-clock TDP, and output clears on SDP 20/40-bit and TDP. Every RAM
input comes straight from a register. Every output bit drives `--fanout`
`dont_touch` LUTs, each with its own capture register; the LUT stops the fitter
from packing the capture register into the M10K output register, which would
turn an unregistered read into a registered one.

`classes.json` covers four fits: `--seed 1`, `--seed 2`, `--seed 3`, and
`--seed 1 --fanout 3`. Across them the unregistered classes did not move at
all, and the registered maxima moved by up to 31 ps with placement. The
fanout-3 fit's maxima are within 2 ps of the other fits'.

```sh
python3 project.py fit1 --seed 1
(cd fit1 && quartus_sh --flow compile top && quartus_sta -t ../dump.tcl top)
for c in 7_slow_1100mv_100c 7_slow_1100mv_-40c MIN_fast_1100mv_100c MIN_fast_1100mv_-40c; do
  python3 arcs.py fit1/char-$c.tsv fit1/arcs-$c.json
done
# ... the same for the other fits, then
python3 classes.py fit1 fit2 fit3 fit4 > classes.json
```

`mistral/tests/m10k_timing.cc` checks `delay.cc` against these classes.
