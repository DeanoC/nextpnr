# Registered SDRAM pad timing regression

This native-flow regression uses the existing `io-registers/ddr.v` fixture,
with DDR capture and SDR data/OE registers on two characterized SDRAM DQ pins.
Its 20ns clock and external delays are synthetic host test budgets.

```sh
python3 mistral/tests/registered-pad/check.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --output /tmp/registered-pad
```

The check verifies successful timing, an output setup failure, primitive clock
rejection despite false-path cuts and `--timing-allow-fail`, and fresh-process
routed checkpoint replay without rereading QSF/SDC. Reports must include
registered capture and OE boundaries; timing aliases must not appear in the
persisted design. Common-engine tests in `io_delay.cc` also cover independent
capture edges/data/OE, pulse distortion, reentry, malformed models and rejection
of unsupported profile settings.

The opt-in fitted reference profile and limitations are documented in
[the IO delay contract](../../../docs/mistral-io-delay.md#gpio-model-boundary).
This regression does not establish physical SDRAM timing or hardware behavior.
