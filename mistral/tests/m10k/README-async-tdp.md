# M10K true-dual-port asynchronous reads

`MISTRAL_M10K_TDP` supports two synchronous write ports with flow-through
`A1Q` and `B1Q` outputs when `CFG_ASYNC_READ=1`. Both addresses remain live
fabric inputs, and each port keeps its own write clock, enable and write-enable
route. The mode is the true-dual-port shape needed by memories with two writes
or two asynchronous read ports, such as the ZX81 media buffer.

Packing reuses the existing M10K BEL and `CFG_TDP=1` configuration. Both
physical output selectors are `ASYNC`; the ordinary TDP clock, core-clock and
enable selectors remain programmed so either port can write. No Mistral device
table changes are required.

Timing treats `A1Q` and `B1Q` as combinational outputs in this mode and adds a
conservative 1.5 ns address-to-output estimate for each port. Write addresses,
data, enables and write enables retain their per-port setup checks. The estimate
is a routing-model value, not a silicon characterization.

Run the host regression with the board files:

```sh
python3 mistral/tests/m10k/async_tdp.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/pins.qsf --sdc /path/to/clocks.sdc \
  --output /tmp/m10k-async-tdp
```

The check is host-only. It synthesizes one direct TDP primitive, routes one
M10K with both clocks and both control-port sets, emits a compressed RBF,
decodes the async selectors and verifies both address-to-Q timing arcs. It
does not establish cross-port collision behavior or hardware initialization.
