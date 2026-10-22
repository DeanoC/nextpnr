# Physical LUT timing regression

`getCellDelay` previously selected LUT delays from the logical input name and
primitive arity. `reassign_alm_inputs` independently assigns those inputs to
physical ALM pins, and `compute_lut_mask` permutes the truth table accordingly.
Renaming inputs while preserving their physical connections could therefore
change timing for an identical bitstream.

An integrated FES RAM Tester checkpoint demonstrates the defect: swapping the
logical D/E names on one ALUT5, its truth-table variables and saved pin-map
entries preserves the complete RBF, but changes ten address-enable setup slacks
from -0.173 ns to +0.130 ns. This is a reporting change without a hardware
improvement.

Bound LUTs now select the existing delay tables using physical pins and the
hardware L5/L6 mux mode. All ordinary LUTs smaller than six inputs, including
buffers and inverters, use L5 mode. The mux-level assignments follow
`get_phys_pin_val`: E selects the final mux in L6 mode, while F selects the final
mux in L5 mode. C/D exchange the middle levels between the two L6 halves, so
their existing early/late envelope is used. Legalisation gives an L5 cell's
last private input F, then E, C/D, B and A, and keeps LUT6 inputs in order.
Yosys times the last ALUT input fastest, so that is where abc9 puts the
critical signal (#214). A last input shared with the other half of the ALM
keeps its own F as well when that half has a free private pin; the other
half reads the net on A/B. Before input legalisation, placement times every
pin legalisation fixes: LUT6 inputs in order and the last L5 input on F. The
other L5 inputs keep the #206 fastest-first scaffold order on E, C/D, B and
A. `--isolated-lut-placement` times them on the isolated legal map instead.
`--arrival-pin-assignment` does not change either estimate; it permutes
private pins after legalisation. Hypothetical cells without nets use the same
order as a fully connected cell; connected cells skip disconnected inputs.
After legalisation, timing uses the actual physical maps under every policy.
The underlying tables retain their existing device, voltage and temperature
limitations.

The LUT's graph input is the native GOUT wire, before the block-input mux.
`getCellDelay` includes that terminal stage as well as the physical LUT-pin
table. Before analogue configuration it uses the existing 175 ps GOUT table
estimate with a zero early bound. At signoff it uses the terminal circuit's
measured delay with the route's propagated waveform. The routed override
subtracts that same measured term, preserving the graph boundary and counting
the mux once. Existing synthetic FF/control inputs already traverse the
terminal circuit and keep their original accounting. Carry and hard-block
input timing are unchanged.

`LutInputDelayTest` exercises a real LD-to-GOUT path through both a native LUT
input and a synthetic control sink. It checks complete analogue delay, the
cell/routing split, cache invalidation, preservation of control delay and
exclusion of the terminal circuit from per-pip calibration observations.

`physical_lut_timing.py` synthesizes L5/L6 functions and an input inversion that
the packer folds into the L5 pin state. Five routed replays rotate every input's
logical name, truth-table variable and physical pin-map entry together. Every
replay must preserve the complete RBF and all reported endpoint delay bounds,
setup slacks and hold slacks. The previous compiler fails this regression.

```sh
python3 mistral/tests/physical_lut_timing.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --output /path/to/results
```

With `BUILD_TESTS` and Yosys available, CTest registers
`nextpnr-mistral-physical-lut-timing` and
`nextpnr-mistral-physical-lut-timing-isolated`. The latter passes
`--isolated-lut-placement` for the fresh route and omits it on all five
checkpoint replays, checking that the physical maps preserve the bitstream and
timing without re-enabling the placement heuristic.
