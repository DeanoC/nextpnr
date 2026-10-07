# Placed JSON pin-state regression

Mistral packing folds constants and supported input inversions into cell pin
states, disconnecting the corresponding constant ports or removing inverter
cells. Those states are part of the logical design. Ordinary placed JSON now
records the existing `FES_PINMAP_V1` snapshots and restores them before resumed
routing. It does not lock the placement as a routed FES scaffold; LAB routing
preparation still runs normally.

`placed_checkpoint.py` synthesizes a two-element arithmetic chain with a folded
input inversion and hard constants, writes a placed checkpoint, resumes routing,
and compares every saved pin state against the resulting routed snapshot. A
frozen routed replay must produce the identical RBF. Four malformed placed
snapshots exercise invalid states, invalid physical pins, incomplete maps and
unsupported versions. The previous compiler fails because the placed snapshot
omits the folded states.

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
Existing routed checkpoints retain their stricter complete-scaffold validation.
