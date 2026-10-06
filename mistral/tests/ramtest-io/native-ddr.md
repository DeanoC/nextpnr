# Native DDR FES RAM Tester diagnostic

The user's FES RAM Tester passes above 130 MHz with Quartus; the best observed
OSS result is 117 MHz. These observations are separate from the following
host-only experiments. No candidate here was programmed or qualified on hardware.

Fixed-rate native DDR builds previously failed packing because their three-output
RAM PLL retained one configured output without consumers. The packer now retains
that counter's configuration without consuming a clock lane. Both 100 and
130 MHz native DDR variants pack, including input DDR, output and OE registers
on all sixteen DQ pads. `budget.py --memory-clock` selects output 1 for the native
100 MHz build; output 0 remains the default for 130 MHz and historical OSS builds.

## Inputs and scope

The copied FES inputs are revision `8f5c5b5982fea01877b223a36cca0024dd15a8c8`.
Both flows use fixed-rate `RAM_RATE_SWEEP`, `RAM_100_ONLY` or `RAM_130_ONLY`,
`RAM_SDRAM_IO_REGISTERS=1` and an all-zero diagnostic BUILD_ID. Neither enables
`RAM_OSS_HIGH_SPEED`. Yosys is 0.69+git5391eeb1e; Quartus is 17.0.2 Build 602.
The Quartus `QUARTUS` macro changes altiobuf's disabled bus-hold spelling from
`OFF` to `FALSE`. The functional controller and capture wrapper match.

This is an explicit extra-output-stage variant, **not the exact RTL/configuration
of the user's hardware-passing Quartus artifact**. Without the extra stage,
OSS packing still rejects the inverted command driver of `SDRAM_nWE`: its
FAST_OUTPUT_REGISTER request requires a directly connected, exclusive FF,
but synthesis places a `MISTRAL_NOT` between that FF and the pad.

The native memory/capture clocks are 100 MHz/417 ps and 130 MHz/2692 ps.
At 100 MHz memory is PLL output 1; at 130 MHz it is output 0. Capture is output 2.
The normal wrapper consumes `dq_fall`; at 130 MHz it adds a rising memory-clock
handoff register. Quartus's fitted low-word handoff launches on a rising capture
clock edge. The simulator below models that low-word alignment explicitly;
FES's simplified `altddio_in` stand-in does not implement this alignment.

## Conditional external timing

OSS routes use CPU analogue timing, seed 2, HeAP beta 10/exponent 2, CPU router
and zero analogue repair rounds. Initial routes allow timing failure and use a
zero-delay SDRAM clock probe to extract physical clock arrivals. That probe is
not a chip timing constraint; its reported Fmax is not a hardware rate ceiling.
Strict frozen-route replays remove the allow-fail setting and add board budgets.

Board assumptions are clock, write-data and read-return flights of 0–0.5 ns,
CS inverter 0–5 ns and 0.2 ns margin. Etron -6 bounds are tAC=6 ns/CL2 at
100 MHz or 5.4 ns/CL3 at 130 MHz, tOH=2.5 ns, tIS=1.5 ns and tIH=0.8 ns.
These are diagnostic assumptions; the installed inverter and board bounds
have not been measured. OSS pad timing uses `QUARTUS_17_0_2_RAMTEST` and
`BOARD_MODEL_FAR_C30P`. Quartus loads every SDRAM output at 30 pF and sets DQ
D1/D3 selectors to zero. All RAM PLL outputs remain in one related clock group
for the Quartus handoff checks. The ordinary FES Quartus recipe cuts those
relationships with separate asynchronous groups, so its timing report has a
different scope.

| Check | 100 MHz | 130 MHz |
| --- | ---: | ---: |
| OSS declared directions | 53/53 | 53/53 |
| OSS checked pad channels | 86/86 | 86/86 |
| OSS memory-domain setup WNS | −14.249 ns | −11.374 ns |
| OSS memory-domain hold WNS | −2.189 ns | −3.343 ns |
| Strict OSS compiler exit | 1 | 1 |
| Quartus external input setup | −14.540 ns | −11.665 ns |
| Quartus external input hold | +9.141 ns | +5.712 ns |
| Quartus external output setup | −2.433 ns | −3.587 ns |
| Quartus external output hold | +1.865 ns | +0.900 ns |
| Quartus capture handoff setup | −0.238 ns | −2.493 ns |
| Quartus capture handoff hold | +1.732 ns | +3.441 ns |

The Quartus minima include all four slow/fast temperature corners, with
32 DDR input paths, 52 output paths and 16 handoff paths per corner per check.
OSS's 86 channels include both DDR capture edges, data and OE for each DQ,
twenty command/address outputs and both clock edges. Coverage does not mean
acceptance. The independent early/late OSS clock envelope also fails to prove
chip clock pulse width. Both flows fail conditional external setup; their
hold results differ. Neither result disproves the existing hardware passes
or establishes a fair frequency comparison against those exact artifacts.

## Read-consumption trace

`native-capture-cycle.sv` instantiates the copied actual controller with the extra
output stage. It models ideal clocks, falling-edge low-word capture aligned to
the next rising capture edge, and the 130 MHz memory-clock handoff. A single read
word appears after CL chip clocks plus tAC and a lumped return delay; other data
is poisoned. It is a cycle diagnostic, not a silicon or board model.

```sh
verilator --binary --timing -Wno-fatal --top-module trace -GRATE=130 \
  --Mdir /tmp/native-cycle-130 mistral/tests/ramtest-io/native-capture-cycle.sv \
  "$FES/cores/fes-ramtest/rtl/sdram_addon_port.v" \
  "$FES/cores/fes-ramtest/sim/board_models.v"
/tmp/native-cycle-130/Vtrace +tac=5.4 +return_delay=0
```

Sixteen runs cover both rates, return delays 0/3/6/9 ns and tAC 0.5 ns or the
rate's maximum. The controller consumes a sample taken only 0.417 ns (100 MHz)
or 2.691 ns (130 MHz, simulator rounding) after nominal data launch. Only the
130 MHz, zero-return-delay, 0.5 ns tAC case captures the valid word; all sixteen
data results agree with their modeled valid window. Physical clock/pad delays
are omitted, so these results motivate tracing the exact consumed cycle with
qualified delays; they do not prescribe a phase or multicycle exception.

## Evidence and next steps

`native-ddr-reference.json` retains input hashes, corner minima, complete budget
assumptions, coverage counts, cycle results and raw artifact hashes. Raw logs,
checkpoints, projects and RBFs remain under `/tmp/nextpnr-full-board-ddr` on
powerboat. The initial routes used the retained initial unused-output compiler;
the final compiler assigns unused counters last. Fresh final pack checks and
strict replays are distinguished in the receipt. Frozen replays retain their
original counter assignments. All-used PLL regression RBFs are byte-identical
to the pre-change compiler, and each unused output routes and replays identically.

Next resolve normal command-inversion packing so the exact passing Quartus RTL
can be compared, then trace the consumed DDR word under qualified clock/pad
delays. Establish chip pulse widths, setup/hold and DQ turnaround before sealing
and testing a hardware candidate. Do not narrow timing bounds or add multicycle
exceptions solely to make these reports pass.
