# Dedicated DDR input registers

This fixture checks the one-bit Cyclone V `altddio_in` path. A primitive with
the checked inactive controls is packed into one `MISTRAL_DDRIN` cell at the
constrained input pad. The high and low outputs use the pad's dedicated
`DATAIN.3` and `DATAIN.2` lanes, respectively; the capture clock uses
`CLKIN.0` and enables the associated DQS16 write-clock path.

The supported profile is width one, `power_up_high=OFF`,
`invert_input_clocks=OFF`, a non-inverted fabric clock, and constant
`inclocken=1`, `aset=aclr=sset=sclr=0`. The data input must be driven directly
by one `MISTRAL_IB`. Dynamic controls, inverted or constant clocks, indirect
data paths, output aliasing, unsupported parameters, and wider primitives are
rejected rather than silently changing the capture behavior.

The Mistral database has no characterized GPIO input-register setup/hold or
register clock-to-Q arcs. `MISTRAL_DDRIN` is therefore excluded from the
timing graph; an empty Fmax section does not establish input-interface timing
closure or timing for logic launched from either captured output. The test is
host-only and does not program a board.

The [oracle](oracle) retains the small Quartus Prime Lite 17.0.2 project, its
compressed RBF, and a mapping/hash manifest. It is a geometry and setting
reference, not a bit-for-bit compatibility claim.

## Reproduce

```sh
python3 mistral/tests/ddr-input/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/ddr-input
```
