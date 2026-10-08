# Placed JSON pin-state regression

Mistral packing folds constants and supported input inversions into cell pin
states, disconnecting the corresponding constant ports or removing inverter
cells. Those states are part of the logical design. Ordinary placed JSON now
records the existing `FES_PINMAP_V1` snapshots and restores them before resumed
routing. It does not lock the placement as a routed FES scaffold; LAB routing
preparation still runs normally.

Placed and routed JSON also record every live clock constraint, including
period, duty cycle, phase group and phase shift, even when the design has no
external IO delays. Resume restores these clocks without rereading SDC against
ports removed by packing. The regression gives the input a 25 MHz constraint
while the default is 50 MHz, and checks both the saved clock table and the
resumed timing report. This catches silent fallback to the default frequency.
Mistral also propagates that constraint through uninverted input and global
buffers without requiring an external IO delay declaration.

`placed_checkpoint.py` synthesizes a two-element arithmetic chain with a folded
input inversion and hard constants, writes a placed checkpoint, resumes routing,
and compares every saved pin state against the resulting routed snapshot. A
frozen routed replay must produce the identical RBF. Malformed placed
snapshots exercise invalid states, invalid physical pins, incomplete maps,
unsupported versions, and a pin-state snapshot present on only some placed
cells. A checkpoint that omits the snapshot on every placed cell still loads
and warns. The previous compiler fails because the placed snapshot omits the
folded states.

Run with an explicit output directory on a filesystem with adequate space:

```sh
python3 mistral/tests/placed_checkpoint.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --output /path/to/results
```

With `BUILD_TESTS` enabled and Yosys available, CTest registers this as
`nextpnr-mistral-placed-checkpoint`, using the build directory for outputs.

Older placed JSON without pin snapshots remains readable with a warning. Its
missing states cannot be inferred from the disconnected ports: regenerate the
checkpoint from synthesis JSON before relying on its logical equivalence.
A checkpoint that annotates only some placed cells is rejected. The omitted
cells would keep JSON defaults and could drop folded constants or inversions.
Existing routed checkpoints retain their stricter complete-scaffold validation.
