# Arrival-based private LUT input assignment

`--arrival-pin-assignment` enables two related heuristics for a fresh Mistral
placement and route. Boolean LUT placement estimates use the physical input
allocation of an isolated cell. After legal ALM input allocation, a single
arrival snapshot assigns later arriving private inputs to faster physical pins.
The option is off by default. A saved JSON setting records provenance and does
not enable the heuristic on a new run without the command-line option.
The option is initialized when the context is created, including Python
`--run` invocations that create their design without `--json`.

Only ordinary two- through five-input Boolean LUTs are permuted. The physical
input set stays fixed; inputs reserved by the other ALM half remain fixed.
Carry mode, LUT6, locked LUTs, and ALMs with direct register input paths are
excluded. Inversion states stay on their logical ports, and bitstream generation
uses the resulting physical maps. Restored scaffold maps are not reassigned.

This is a setup-oriented placement/routing heuristic. Its arrival snapshot is
not recomputed after each permutation. It does not replace final routed timing
or guarantee a setup or hold improvement. Choose it per design/profile and
qualify the final setup and hold results. No timing bounds, clock constraints,
placer exponent, or candidate budgets are changed by this option.

For example, add `--arrival-pin-assignment` to an existing complete nextpnr
command. A routed checkpoint can be replayed without the option: its recorded
physical input mappings are sufficient to reproduce the bitstream and timing.

The architecture tests in `lab_pinmap.cc` check isolated placement estimates on
both halves, and verify that shared inputs, inversion, register paths and BEL
locks survive private-input optimization. `physical_lut_timing.py` additionally
checks bitstream and endpoint timing invariance under logical port permutations.

## Controlled prototype evidence

The study used main `434b8664`, including the physical LUT6 E/F correction.
The prototype compiler hash was
`e342237b527abc775b66b42ff5964cf91a99cb4db15dd470244bd5cb5d26df2f`.
These results explain the explicit opt-in; they are not measurements of a
hardware clock limit or qualification of later revisions.

| Design/profile | Main | Prototype |
| --- | --- | --- |
| Pong, original defaults, seeds 1–8 | 8/8 pass 74.25 MHz | 7/8 pass; seed 7 fails at 67.63 MHz |
| C64 seed 5, timing weight 2000, exponent 5: system | 51.06 MHz, −0.437 ns | 52.95 MHz, +0.264 ns |
| Same C64 trial: video | 76.40 MHz | 78.68 MHz |
| Same C64 trial: runtime | 545.57 s | 206.18 s |

All reported C64 hold margins were positive. The passing prototype route
replayed with the main compiler reproduces its exact bitstream, clock fmax,
and setup/hold summary. The same RTL netlist and constraints were used;
no RAM location hint was applied. Source netlist SHA256 prefixes were
`8f1140bd2516a055` (Pong) and `225ca7674734fd3e` (C64).

## Current-main qualification

The explicit CLI implementation was rebuilt from main `6053c560`, including
PR175. Compiler SHA256 was
`06c3861dbe9c7569eb94a97cce3230fef56efba3f33d4596be66e6c4992cf82f`.
Five architecture tests, two CLI policy tests, and five physical LUT
permutation cases passed. A routed checkpoint generated with the option
replayed without it with identical bitstream and full endpoint timing.

On the original C64 netlist, timing weight 2000, exponent 5, GPU 0:

| Seed | System setup margin | Result |
| --- | --- | --- |
| 1 | −0.268 ns | System fails; video and all holds pass |
| 2 | Unavailable | 1200 s timeout |
| 3 | −0.729 ns | System fails; video and all holds pass |
| 4 | +0.038 ns | All clocks pass setup and hold |
| 5 | +0.575 ns | All clocks pass setup and hold |

Seed 4 achieved 52.33 MHz system and 83.93 MHz video in 302.11 seconds.
Seed 5 achieved 53.84 MHz system and 80.74 MHz video in 179.92 seconds.
Two of five seeds pass all setup and hold clocks; one times out. The
matched option-disabled seed 4 control on GPU 0 timed out at 1200 seconds.
On GPU 1, seed 5 with the option disabled achieved 49.35 MHz system
(−1.115 ns setup) in 520.66 seconds; enabling it achieved 53.84 MHz
(+0.575 ns setup) in 182.17 seconds, with all setup and hold clocks passing.
These controls use the same netlist, constraints, seed and placer profile.
These numerical results predate the later main merge and CLI-hook fix.

A supplemental run on a newer Pong netlist (`5c98f1fc35188c74`, FES
`d2d1a0ea`) with the option **disabled**, GPU 1, passed seven of eight seeds;
seed 8 missed setup by 0.336 ns. That input differs from the original issue
netlist and must not be combined with the original 8/8 main result.
