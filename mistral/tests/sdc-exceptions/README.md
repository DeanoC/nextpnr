# SDC clock exceptions

Clock-level `set_clock_groups`, `set_false_path`, and `set_multicycle_path`
in the shared SDC parser. Patterns (`*` and `?`) match a `create_clock -name`
or a clock net name when timing analysis sees the clock, so PLL outputs named
during packing are covered. `-name` and `-period` may follow the target.
A cut pair is omitted from setup WNS even when the clocks share a driver.

`set_clock_groups` accepts `-asynchronous`, `-exclusive`,
`-logically_exclusive`, and `-physically_exclusive`. All four cut timing
between clocks in different groups. One `-group` is exclusive with every
clock outside it. `set_false_path` between clocks cuts only the named
direction. Both commands cut hold as well as setup, including a false path
from a clock to itself and a phase-related pair. `set_multicycle_path -setup N`
widens the setup window by `N - 1` capture periods (`-start` uses the launch
period). A clock with no constraint uses the target period, so the exception
does not crash. Hold stays on the single-cycle edge: `-hold` is accepted and
ignored, including when it shares a command with `-setup`. Cell, pin, and net
multicycle targets are rejected.

The fixture is two phase-related 50 MHz PLL outputs (0° and 90°) on
`5CSEBA6U23I7`, with a deep path on `clocks[0]` and crossings both ways.
Host-only. No bitstream compare.

```sh
python3 mistral/tests/sdc-exceptions/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --output /path/to/sdc-exceptions-results
```
