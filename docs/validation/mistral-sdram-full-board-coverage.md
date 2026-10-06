# SDRAM registered-path coverage at 100/130 MHz

The retained full-IO-register diagnostic designs have declarations for all
53 active SDRAM directions and setup/hold checks for all 70 expected pad
channels. Both strict replays fail timing. This establishes check coverage,
not a hardware-qualified full-board build or acceptance at 130 MHz.

These are the historical `RAM_OSS_HIGH_SPEED` diagnostic fixtures with forced
DQ input/data/OE packing, not the address-only hardware-tested artifacts in
[issue #135](mistral-sdram-issue-135.md). Neither production FES constraints
nor controller RTL changes in this follow-up. The user's passing Quartus
image is separate evidence; these failing diagnostics do not contradict it.

## Report and audit behavior

Detailed endpoint records now identify setup/hold gate participation and
slack separately from arrival intervals. Clock cuts can leave a reported
arrival while `setup_checked` and `hold_checked` are false and their slacks
are `null`. Setup-only analysis reports no hold check. Eligibility follows
the existing timing gate, including related-clock handling and SDC cuts;
the reporting change does not alter timing decisions or routing.

`mistral/tests/ramtest-io/path_coverage.py` reads the checkpoint and report
from the same invocation. It checks the qualified profile, declaration,
pad identity, external reference event, register clock/edge, finite arrival
interval, and finite setup/hold slack for every expected channel. It rejects
missing OE channels, missing capture/forwarding edges, unsupported pad modes,
clock cuts, setup-only results and non-final reports. It normalizes clock
aliases through checkpoint net bits. Older reports without check annotations
cannot establish this coverage.

The 70 channels comprise 16 SDR input captures, 16 DQ data outputs, 16 DQ
output-enable outputs, 20 address/bank/command outputs and two forwarded-clock
edges. The two structurally constant mask outputs remain exempt. Coverage
exit 0 means checks exist; `all_channel_slacks_nonnegative` separately reports
whether those checked channels pass. Neither field replaces the compiler
exit status, complete design timing or board qualification.

## Strict retained-route results

Both runs use the same illustrative board assumptions as the retained
[full-board investigation](../../mistral/tests/ramtest-io/README.md): 0..0.5 ns
clock/data/read flights, 0..5 ns chip-select inverter propagation and 0.2 ns
margin. They retain the 30 pF reference pad envelope and corrected native
zero-selector input model. These assumptions are not measured board bounds.

| Rate | Compiler exit | Directions declared | Channels checked | Pad setup WNS | Pad hold WNS |
| --- | ---: | ---: | ---: | ---: | ---: |
| 100 MHz | 1 | 53/53 | 70/70 | −7.578 ns, nCS data | −2.189 ns, BA1 data |
| 130 MHz | 1 | 53/53 | 70/70 | −11.384 ns, DQ7 capture | −3.343 ns, BA1 data |

All BEL placements and nonempty routing bindings are unchanged on checkpoint
replay. Whitespace-only input-buffer route attributes disappear on export;
they contain no routing arcs. The compiler writes diagnostic RBFs despite
timing failure; those files are not accepted artifacts and were not loaded
onto hardware. A preliminary replay without RBF generation produced only
non-final timing and was correctly rejected by the audit.

`mistral/tests/ramtest-io/path-coverage-reference.json` records compiler/source
hashes, commands, complete final timing summaries and artifact identities.
Raw evidence is retained on powerboat under
`/tmp/nextpnr-full-board-coverage/{100,130}`. The compiler build is
`/tmp/nextpnr-full-board-build`, based on merged main `5063215e` plus the
reporting changes in this branch, using the CPU build configuration.

Validation covers the IO-delay unit suite, setup-only/cut report cases and
11 audit rejection/qualification checks against each actual routed fixture.
The complete direction audit and subchannel checks pass at both rates;
negative timing remains an explicit failure.

## Remaining evidence

An additional isolated synthesis/packing probe selects the existing FES DDR
input branch with `RAM_SDRAM_IO_REGISTERS=1`, without `RAM_OSS_HIGH_SPEED`.
Both 100/130 MHz synthesise successfully. Both pack logs show all 16 DDR
captures merging into bidirectional IO cells, before packing exits 125:
the three-output PLL retains an unused 75 MHz output at 100 MHz, or an
unused 100 MHz output at 130 MHz. `setup_plls()` currently requires every
configured output to feed clock buffers and rejects those unused outputs.
This is a packing blocker, not a routed timing result. No packed checkpoint
or bitstream is produced. Raw inputs/programs/logs are under
`/tmp/nextpnr-full-board-ddr`; their hashes accompany the coverage receipt.
The diagnostic output-register define changes the controller latency from
the ordinary Quartus recipe, so this probe is not a matched hardware build.

The historical read-window audit has no common capture window under its
declared bounds. Choosing a phase or narrowing a timing envelope solely to
make STA pass would not repair that evidence. The standard Quartus DDR-input
branch has different PLL clocks and controller handoff stages from
`RAM_OSS_HIGH_SPEED`; selecting it requires a matched RTL/capture-cycle
comparison and native packing validation.

Full-board acceptance still requires qualified clock/data correlation and
electrical/board bounds, the actual inverter identity/load/slew, forwarded
clock waveform checks, DQ bus turnaround, controller sample consumption and
controlled hardware validation. This follow-up supplies the audit needed to
keep omitted checks distinct from negative checks while doing that work.
