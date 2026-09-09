# Dedicated SDR input registers

This fixture checks the opt-in Cyclone V GPIO input register path. A
width-one `MISTRAL_IB` whose output drives exactly one `MISTRAL_FF.DATAIN`
can be packed when the input pin has `FAST_INPUT_REGISTER ON`. The packed
cell is `MISTRAL_SDRIN`; the fabric flip-flop is removed and its `Q` net is
driven by GPIO `DATAIN.3`. The register clock uses GPIO `CLKIN.0` and the
associated DQS16 write-clock enable and inverted-clock settings.

The supported form has constant inactive controls (`ENA=1`, `ACLR=1`,
`SCLR=0`, and `SLOAD=0`), a directly connected data input, a non-inverted
clock, and no register parameters. `FAST_INPUT_REGISTER OFF` leaves the
ordinary input buffer and fabric flip-flop in place. Dynamic controls,
inverted clocks, data fanout, unsupported parameters, and unsupported GPIO
sites are rejected instead of silently changing the capture behavior.

The Mistral database has no characterized GPIO input-register setup/hold or
clock-to-Q arcs. `MISTRAL_SDRIN` is therefore excluded from the timing graph;
the host report's empty Fmax section does not establish input-interface timing
closure or timing for logic launched from the GPIO register's `Q`. The fixture
checks routing, utilization, compressed RBF generation, and the exact DQS/GPIO
settings observed in the Quartus 17.0.2 oracle. It is a host-only test and does
not program a board.

## Reproduce

```sh
python3 mistral/tests/sdr-input/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/sdr-input
```

The [oracle](oracle) contains the small Quartus Prime Lite 17.0.2 project,
the retained compressed RBF, and a settings/hash manifest. Decompress the RBF
and run `mistral-cv decomp 5CSEBA6U23I7` to inspect it; the oracle is a mapping
reference, not a bit-for-bit compatibility claim.
