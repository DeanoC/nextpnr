# Direct Intel `altiobuf_*` primitives

This fixture checks the narrow direct-buffer profiles emitted by the pinned
Yosys `synth_intel_alm` flow for Cyclone V. nextpnr keeps the constrained
`MISTRAL_IB`, `MISTRAL_OB`, and `MISTRAL_IO` BELs from Yosys and folds the
corresponding `altiobuf_in`, `altiobuf_out`, and `altiobuf_bidir` cells into
those existing GPIO cells. The bidirectional output path is attached to the
GPIO `MISTRAL_IO.O` port and remains connected to its fabric output users.

The supported profile is one channel with bus hold and differential mode
disabled. The output profile also requires `use_oe=FALSE`; bidirectional OE
may be a fabric net or a hard constant. Input and output data must be directly
connected to their matching Mistral GPIO buffer, while bidirectional `dataio`
must be the direct user of one `MISTRAL_IO.I` path. Wider channels, unsupported
parameters, malformed direct-pad topology, and other profiles are rejected
before placement instead of being silently dropped.

The host check synthesizes the Verilog fixture, routes it on
`5CSEBA6U23I7`, checks the normalized JSON and utilization, and requests a
compressed RBF. It is host-only and does not program a board; GPIO electrical
timing is outside the current Mistral timing model.

## Reproduce

```sh
python3 mistral/tests/altiobuf/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --output /tmp/altiobuf
```
