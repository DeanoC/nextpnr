# Clock-edge routing intervals

`--mistral-clock-edge-ranges` selects each registered launch and capture
event's own rising or falling route interval. It is default-off and requires
`--mistral-routed-delay-ranges`. Both selections are recorded in checkpoints
and require explicit acknowledgement on reload. The clock-edge setting also
belongs to critical-cohort calibration identity.

The ordinary interval envelope mixes the earliest rising/falling delay with
the latest rising/falling delay. On a clock route with rising delay 1 ns and
falling delay 3 ns, that envelope can create -2 ns of skew even when both
registers observe the same rising edge on identical clock routes. The
selected mode keeps those transitions separate. Rising/rising, falling/falling
and opposite-edge paths use their actual clock events in forward arrival,
backward required time, unrelated-clock cancellation and reported path sums.
Data intervals, clock periods/phases, endpoint admission and primitive pulse
requirements keep their existing behavior. A manually supplied route interval
applies to both transitions; it replaces earlier routed clock bounds.
Changing clock-edge mode requires a fresh routing-delay update; a cached
`run(false, ...)` rejects a changed mode before evaluating stale intervals.

This is transition selection within supplied intervals. It adds no common-path
pessimism removal, clock reduction, exception, board budget or physical corner
qualification. A default-false architecture capability preserves other
backends and ordinary Mistral behavior.

The focused regression supplies distinct early/late rising and falling clock
routes between physical owners, plus an independent 1..4 ns data route. It
checks all four launch/capture edge combinations, native endpoint/path sums,
a remaining setup failure, manual clock overrides, re-entry and rejection of
stale endpoint queries after a mode change. The CLI regression checks default
behavior, dependency/conflict rejection and explicit checkpoint selection.

```sh
build/nextpnr-mistral-test --gtest_filter='IoDelayTest.ClockEdgeRanges*'
ctest --test-dir build -R clock-edge-ranges-cli --output-on-failure
```
