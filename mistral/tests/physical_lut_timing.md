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
their existing early/late envelope is used. Before input legalisation, the
default uses the fastest-first placement scaffold. `--isolated-lut-placement`
uses C, E, F, B, A for an isolated L5 cell and A through F for LUT6.
`--arrival-pin-assignment` does not change this estimate; it permutes private
pins after legalisation. Hypothetical cells without nets use the same
order as a fully connected cell; connected cells skip disconnected inputs.
These estimates affect placement QoR and remain opt-in following #206.
After legalisation, timing uses the actual physical maps under every policy.
The underlying tables retain their existing device, voltage and temperature
limitations.

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
