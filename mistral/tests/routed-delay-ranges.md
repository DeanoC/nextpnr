# Routed early/late delay propagation

`--mistral-routed-delay-ranges` retains the backend's supplied route intervals
for every internal data and clock path, independently of external IO
constraints. It is default-off and recorded in checkpoints; replay requires
the option again.

Ordinary STA historically wraps the maximum route delay into both ends of a
`DelayPair`. External IO timing and declared primitive clocks already use the
full `DelayQuad` interval. Consequently, adding an IO constraint can change
internal hold and clock-skew checks on an otherwise unchanged layout. A
1..4 ns route becomes 4..4 ns in the ordinary mode, whereas this option keeps
1..4 ns. It can reveal a short hold path hidden by the scalar approximation.

The common architecture capability defaults to false. Only Mistral's
explicit option enables it; existing external IO and primitive-clock behavior
is unchanged. The option does not enable external IO timing, add constraints,
qualify a fast corner or improve placement. Cell/clock/pad/board bounds still
require their own qualification.

Build the Mistral backend with tests, then run:

```sh
build/nextpnr-mistral-test \
  --gtest_filter='IoDelayTest.RoutedRangesDoNotDependOnExternalIoConstraints:IoDelayTest.PrimitiveClock*'
ctest --test-dir build -R routed-delay-ranges-cli --output-on-failure
```

The internal-route regression supplies independent 1..4 ns bounds between
physical FF owners. It verifies the early hold change, unchanged setup with
fixed clocks, equality with the existing IO mode and restored default
behavior after re-entry. The CLI regression checks default-off behavior,
conflicting settings, recording and explicit reload acknowledgement.
