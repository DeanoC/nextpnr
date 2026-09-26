# Quartus constraint compatibility

This fixture exercises the small Quartus SDC/QSF subset accepted by
nextpnr-mistral.  It keeps the input clock constraint, while accepting
`derive_pll_clocks`, `derive_clock_uncertainty`, and asynchronous
`set_clock_groups` as compatibility metadata.  PLL packing already derives
the generated clocks from the `altera_pll` cell, so these commands do not
create duplicate constraints.  The QSF fixture also uses Quartus's `-entity`
qualifier on `set_instance_assignment`; nextpnr applies the assignment to the
selected top-level object. The HPS I2C regression additionally accepts an
internal hard-block `HPS_LOCATION` assignment and converts it to a BEL
constraint.

Run it with the locked toolchain executables:

```sh
python3 mistral/tests/quartus_constraints/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --output /path/to/quartus-constraints-results
```

The check is host-only.  It routes one 25 MHz PLL output from the 50 MHz
DE10-Nano reference and writes a compressed RBF for parser and timing
regression coverage.
