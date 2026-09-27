# FES menu DDR seed-1 GPU routing regression

`synth.json.gz` is the exact Yosys output retained for DeanoC/fes issue 266,
from FES commit `3da61eb9` on `feat/native-menu-scanout`. The QSF and SDC are
the recipe's inputs from the same commit. The fixture has a single 74.25 MHz
pixel domain and uses HPS DDR port 0 for a read-only menu diagnostic.

On nextpnr `f60b33aa`, `--router gpu --gpu-device 0 --seed 1` stops after
40 iterations with `TD.31.8.41` at occupancy 2. Run the regression with:

```sh
python3 mistral/tests/gpurouter/menu_ddr_seed1.py \
  --nextpnr build/nextpnr-mistral --output /tmp/menu-ddr-seed1
```

The test requires zero overuse, final architecture binding, routed JSON,
timing report and an RBF. A provisional table-model Fmax from an illegal
route does not pass.
