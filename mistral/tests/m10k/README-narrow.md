# Narrow true-dual-port M10K

The Cyclone V M10K has equal-width true-dual-port geometries for 8192x1,
4096x2 and 2048x5 memories. `setup_tdp_m10k()` accepts those geometries and
maps their narrow data lanes through the same bank replication used by the
simple-dual mapper. The 8192x1 mode also uses the dedicated data-lane wiring
for the thirteenth address bit. Scalar one-bit Yosys ports are mapped to the
physical `DATAAIN`/`DATABIN` and `DATAAOUT`/`DATABOUT` pins so placement and
routing do not depend on vector indexing.

The locked Yosys M10K TDP inference rules still emit only the 10- and 20-bit
forms. The regression therefore instantiates `MISTRAL_M10K_TDP` directly and
checks all three native narrow modes at the nextpnr JSON boundary. Mixed-width
TDP remains limited to the characterized 10/20-bit geometries.

Run the portable host regression with the DE10-Nano board constraints:

```sh
python3 mistral/tests/m10k/narrow.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf mistral/tests/pll/pins.qsf --sdc mistral/tests/pll/clocks.sdc \
  --output /tmp/m10k-narrow
```

Each case must pack one `MISTRAL_M10K`, produce a compressed RBF, route both
TDP clocks and control pins, and meet the 50 MHz constraint. This is host-only
evidence; no narrow TDP bitstream has been accepted on the designated kit.
