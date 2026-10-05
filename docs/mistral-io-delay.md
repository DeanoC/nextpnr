# Mistral input and output delay constraints

`set_input_delay` and `set_output_delay` attach external timing requirements to
top-level ports. Values are in nanoseconds. They participate in STA, placement
slack, router criticality, timing reports and the existing bitstream timing gate.
This support was introduced while investigating nextpnr issue #135: passing
internal SDRAM-clock Fmax did not time the SDRAM pad interface.

```tcl
create_clock -period 20 -name memory [get_ports clk]
set_input_delay  -clock memory -min 1 [get_ports din]
set_input_delay  -clock memory -max 3 [get_ports din]
set_output_delay -clock memory -min 0 [get_ports dout]
set_output_delay -clock memory -max 4 [get_ports dout]
```

These values belong to the small host regression fixture; they are not SDRAM
constraints. SDRAM values must be derived from the actual device specification,
board propagation delays and the emitted clock relationship.

An input minimum/maximum specifies the earliest/latest external data arrival
relative to the reference clock edge. An output maximum consumes setup budget;
an output minimum controls hold. A negative output minimum therefore requires
the old data to remain stable after the reference edge. Input delays appear as
`source` segments, output maxima as `setup`, and output minima as `hold`.

The clock can be a `create_clock -name` name, an exact physical net/alias name,
or a single `[get_clocks ...]` result. PLL clock nets can be selected before
packing; their period and phase are validated after PLL clocks are derived.
For example, `-clock [get_clocks {ram_clock.clocks[1]}]` can reference the
PLL-derived shifted output. Do not add a separate `create_clock` on a shifted
PLL output: Mistral derives that phase from the PLL parameters.

`-clock_fall` references the falling clock edge. Both minimum and maximum must
be supplied, either through separate `-min`/`-max` commands or by omitting both
options to assign the same value to both. Subsequent assignments replace the
specified bound for the same clock/edge. A reversed interval is rejected.
`get_ports` supports literal bus names, brace-enclosed lists, `*` and `?`.
Brackets in bus names are literal; `{SDRAM_DQ[*]}` selects the bus bits.

The initial implementation supports one clock/edge per port and direction, and
the same clock or equal-period phase-related clocks. Independent input/output
constraints can coexist on a bidirectional port. For `MISTRAL_IO`, output
constraints cover both data and OE. Virtual clocks, `-add_delay`, separate
data rise/fall constraints, and differing-frequency reference clocks are not
supported. Unsupported options, empty selections, wrong port directions and
missing timing boundaries fail explicitly. Clock-level `set_false_path`, clock groups and setup multicycles use the
merged SDC timing-exception implementation. Clock uncertainty remains a
compatibility no-op; multicycle hold retains the single-cycle relationship.

Constraints and the clock periods, duty cycles and phase relationships used by
their analysis are preserved in JSON settings. Explicit clocks on input pads
propagate through uninverted input/global buffers when IO delays are enabled.

## GPIO model boundary

Mistral supports these constraints on unregistered `MISTRAL_IB`, `MISTRAL_OB`
and `MISTRAL_IO` fabric interfaces. Registered SDR/DDR GPIO modes are rejected for external constraints because
their pad capture and clock-to-pad models and bidirectional timing boundaries
are incomplete. A qualified reference profile now times some fabric-facing
GPIO register arcs; see [the characterization fixture](../mistral/tests/gpio-timing/README.md).
This does not enable external pad constraints on registered modes.

The data timing boundary is the GPIO routing ingress/egress, not a newly
characterized package-pad model. Checks use the existing routing/cell delay
models, including their minimum/maximum ranges. The analogue model remains
the backend's existing slow-corner model; its minimum interval is not an
independently characterized fast-corner hold bound. External clock-pad latency,
unmodeled GPIO/pad delays and board delays must be accounted for separately.
Accepting constraints and passing these checks do not establish SDRAM hardware
acceptance. There is no SDRAM board model or automatic SDRAM phase selection.

## Verification

`IoDelayTest` covers input/output setup budgets, negative output hold, falling
edges, phase relationships, clock skew, bidirectional OE, rejected constraints
and JSON reload. Run the synthesized CLI fixture with:

```sh
python3 mistral/tests/io-delay/check.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --output /tmp/mistral-io-delay
```

It checks a passing build and deliberately failing input setup, output setup
and output hold budgets through routing and bitstream generation. It retains
logs, constraints, reports and a results receipt. It also reloads the passing
checkpoint in a fresh process without an SDC file and verifies the external
requirements in its timing reports. It does not program hardware.
