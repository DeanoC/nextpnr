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
Constrained checkpoints keep a width-one `dq[0]` root port's literal name so
it cannot reload as scalar `dq` and lose its saved constraints.

## GPIO model boundary

Mistral supports these constraints on unregistered `MISTRAL_IB`, `MISTRAL_OB`
and `MISTRAL_IO` fabric interfaces. An explicit reference profile also supports
registered pads on the tested DE10-Nano SDRAM pins. Enable it with a QSF
instance assignment on each pad:

```tcl
set_instance_assignment -name NEXTPNR_GPIO_TIMING_PROFILE QUARTUS_17_0_2_RAMTEST -to {SDRAM_DQ[*]}
set_instance_assignment -name BOARD_MODEL_FAR_C 30P -to {SDRAM_DQ[*]}
```

The profile uses the retained Quartus 17.0.2 four-corner reference fits for
`5CSEBA6U23I7`, default delay chains, 3.3-V LVTTL, 16mA, fast slew, no bus
hold, pull-up or clamp, inactive asynchronous clear and constant clock enables.
It requires the tested package pin assignment and a matching placed BEL.
This is an opt-in fitted reference envelope, not board or silicon signoff.
The [characterization fixture](../mistral/tests/gpio-timing/README.md) records
its raw evidence, coverage and limitations.

Supported input capture is `MISTRAL_SDRIN`, `MISTRAL_DDRIN` or
`MISTRAL_SDRIO` with `IOREG_IN=1`, on the 16 tested DQ pins. SDR capture
has one rising-edge pad check; DDR capture (`IOREG_IN_DDR=1` in SDRIO) has
independent checks for both capture edges. Supported output is `MISTRAL_SDROUT` on the tested
DQ/address/bank/command/mask pins, or `MISTRAL_SDRIO` on DQ with both data
and OE registered. Output timing includes the output buffer and declared load;
`BOARD_MODEL_FAR_C` must specify a finite 0..30pF in farads (e.g. `3e-11`)
or with suffix `P`/`p`. Every supported load uses the conservative 30pF late
bound and a zero early bound. Load values are declarations, not measurements.

The same profile supports constant `MISTRAL_DDROUT` clock forwarding on
AD20 with a declared supported load. It models both pad edges independently:
`DDR_HIGH=1` makes the pad rise on the fabric rising edge; `DDR_HIGH=0` makes
it rise on the fabric falling edge. Maximum mux-ingress-to-pad delays are
5.400ns for fabric rising launches and 5.380ns for falling launches, with
conservative zero early bounds. The native clock route is separate. The
profile also enforces the primitive clock period/pulse requirements.

These arcs allow output delay constraints and pad-edge timing reports; they
do not automatically create an SDC generated clock on the output pad. SDRAM
budgets referenced to the fabric clock must include the actual forwarded
clock route, mux/pad latency and board propagation exactly once. A zero early
bound is conservative and may prevent closure where a correlated corner
model would pass. Clock waveform requirements at the memory chip remain a
separate board-level check.

Mixed registered/combinational data and OE, fabric-data DDR output, other pins/devices/electrical settings and nondefault delay chains
have no complete pad model and reject external constraints.

The common timing analyzer now has separate registered-pad read and write
boundaries. Architectures provide complete external relationships through
`getRegisteredIoTiming(cell, pad, input)`: each named capture edge has its own
setup/hold checks, and each data/OE launch channel has its own clock-to-pad
interval. These models refer to the real cell's clock routing ingress. They
must include the pad buffers, electrical/load conditions and any internal
delay chains; fabric-facing register arcs cannot substitute for them. A
direction containing unsupported combinational paths must return no model.

The analyzer creates private ports named `PAD$timing$read$...` or
`PAD$timing$write$...` on the existing cell identity. These aliases exist only
inside STA. They do not change cell ports, physical net drivers/users, routes,
bitstreams or checkpoints. Every capture edge uses a separate node so rising
and falling timing checks cannot overwrite each other. Their paths use the
normal setup/hold, phase, skew and SDC exception machinery. The private bridge
has no routing segment: its physical delay is entirely in the supplied model.
Detailed net reports include each alias's logical source and clock event;
the physical driver is null for an externally driven pad net.

Primitive clock requirements are checked independently of data-path timing.
The architecture API `getPrimitiveClockRequirements` supplies period and high/
low pulse limits at the real clock routing ingress. GPIO limits include local
clock CELL early/late distortion from the Quartus reference. STA subtracts
the routed clock delay range from each pulse width, conservatively bounding
rise/fall distortion, and requires an explicit physical clock waveform.
These checks also run with manual route updates, setup-only analysis, clock
skew disabled and data paths cut. A violation is a hard error, including with
`--timing-allow-fail`: the primitive timing model is outside its qualified
waveform domain. It is not an ordinary relaxable data-path slack violation.
Clock uncertainty remains unsupported and must be covered in the supplied
waveform/budgets when assessing a physical board.

For unregistered interfaces the data timing boundary is the GPIO routing
ingress/egress, rather than the reference profile’s complete pad boundary. Checks use the existing routing/cell delay
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
