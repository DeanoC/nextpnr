# Native GPIO timing reference

`--mistral-gpio-core-reference` selects a bounded, default-off local SDR
GPIO model for `5CSEBA6U23I7`. It covers standalone SDR output registers
and bidirectional pads with all three SDR input, output and OE registers.
Every selected pin must have a retained fitted reference. Dynamic enables,
clears, inverted clocks, DDR capture and changed electrical/delay settings
are rejected. Both clocks require exact periods of at least 10 ns and a
nominal 50 percent duty cycle. Saved checkpoints require the flag again.

The profile checks the package-pin/BEL owner, physical DQS selectors,
architecture-owned port mappings and net-owned routing cuts after RBF
configuration. Model and endpoint discovery precede clock inference; every
selected GPIO therefore declares mandatory primitive-clock checks, even
without a pad-profile attribute. Final STA requires the inferred waveform,
and its same-edge period must meet the model's 10 ns domain boundary.
A missing analogue
observation on a selected data, Q or clock path is fatal.

## Measured boundaries

The old 120/310 ps fabric requirements and 0..870 ps Q approximation use
bare DDIO CELL differences. They exclude local IO_RE/HMCPHY_RE segments.
These references retain the full routing rows and use the actual physical
port identities, including HMC PHY bypass aliases:

| Port | Native cut | Local reference |
| --- | --- | --- |
| I/OE | GOUT before the block-input buffer | Data path after the matching BLOCK_INPUT_MUX output through the storage CELL |
| CLK/CLKIN | DCMUX clock ingress | Matching zero-delay GLOBAL_LOCAL_SEL marker through the local clock interconnect and clock CELL |
| Q | GIN before its VIO/HIO buffer | Full clock prefix, clock CELL, Q CELL and return interconnect, stopping before the matching VIO/HIO buffer |

The selected data/OE route simulates its terminal GOUT buffer once. The
clock prefix remains in the local reference; no second terminal DCMUX
simulation is appended. Q excitation uses Mistral's explicit original
GPIO DATAIN.3 source API, which verifies the routing node even when reverse
lookup reports HMC. The ordinary rnode-only waveform API is unchanged.

For a native data arrival, setup is `data_local - clock_local - uTsu` and
hold is `clock_local + uTh - data_local`, retaining the signs of TimeQuest's
reported adjustments. These equations involve five rounded quantities;
requirements receive an outward 3 ps guard. Q and clock-prefix intervals
receive a 1 ps guard for two rounded totals. Negative setup/hold values
are preserved.

For example, V12's slow100 Q reference is 1734..1905 ps rising and
1830..2010 ps falling. AE23's HMC bypass reference is 2465..4389 ps rising
and 2435..4358 ps falling. Their native I hold requirements are respectively
468 ps and -83 ps. A single broad CELL-only bound does not represent both.

The existing pad and primitive-clock envelopes start at the internal
register clock ingress. Under this flag their bounds are translated to
the same native DCMUX cut using independent rising/falling, early/late
clock-prefix queries. Pad output early bounds stay zero. This translation
does not newly qualify those legacy pad envelopes or add a board budget.

## Retained evidence and checks

The fixture has 38 physical package pins: 38 output-data, 16 OE and 16 input
register roles. The archive contains 1376 timing observations (two q[]
probes for each input register) and 1120 independent clock-prefix
observations. Every role has both data transitions in setup/hold and both
clock transitions in early/late analysis at all four retained corners.
Raw reports, source/SDC/QSF inputs and the fitted RBF are compressed and
hash-bound in `manifest.json`. The parser re-extracts all observations and
independently reconstructs register-to-pin bindings from the pad reports.

Generate or check the compiled table:

```sh
python3 mistral/tests/gpio-core/model_reference.py --check \
  --config-reader build/mistral/nextpnr-mistral-gpio-core-config
python3 mistral/tests/gpio-core/test_reference.py
ctest --test-dir build -R gpio-core --output-on-failure
build/nextpnr-mistral-test --gtest_filter='GpioCoreTimingTest.*'
```

Reproduce the fit and all reports in a fresh directory:

```sh
python3 mistral/tests/gpio-core/characterize.py \
  --quartus-bin /path/to/quartus/bin --output /fresh/reference-run \
  --config-reader build/mistral/nextpnr-mistral-gpio-core-config
```

The public profile uses the native engine's slow100 table only. The other
three corners are retained data for a separately qualified timing engine.
This is host characterization, not whole-chip fast-corner qualification,
SDRAM board qualification or hardware acceptance. No timing exceptions,
clock reductions or physical bitstream changes are introduced by the flag.
