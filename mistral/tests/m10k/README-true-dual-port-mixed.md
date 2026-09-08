# Mixed-width true dual-port M10K

Use the paired Yosys fork's explicit `ram_style="m10k_tdp_mixed"` style for
independently clocked read/write ports of different widths. Physical widths
are 10 and 20 bits (1024 and 512 words); 8/16-bit payloads are padded within
each 10-bit lane. Tagged equal 10/10 and 20/20 configurations are also supported.
Each write replaces the whole port word and returns NEW_DATA on that port.
Each enable gates reading and writing. There is no reset or byte mask in this
mode. The existing `m10k_tdp` and `m10k_tdp_byte` styles retain their contracts.

Storage consists of 1024 ten-bit lanes. Wide address N comprises narrow
addresses 2N and 2N+1, with the lower address in the least-significant bits.
A narrow write preserves the neighboring lane. Cross-port accesses that
**overlap physical storage**, with at least one write, are undefined even
when clocks are shared. Numerical address inequality does not ensure safety:
narrow address 3 overlaps wide address 1. There is no writer priority.

## Primitive and physical mapping

`MISTRAL_M10K_TDP` uses `CFG_ABITS`/`CFG_DBITS` for port A and
`CFG_RD_ABITS`/`CFG_RD_DBITS` for both reads and writes on port B. Set
`CFG_MIXED_WIDTH=1` and supply both B parameters explicitly. Each geometry
must be 1024x10 or 512x20. `INIT` holds canonical 10-bit lanes in ascending
address order. Port names and timing contracts are unchanged. Packing uses
the existing M10K BEL; no Mistral database or BEL additions are required.

For unequal widths, A1EN/B1EN route to ENABLE.0/.1, and TOP_CE0_SEL/BOT_CE0_SEL
are 0/1. A10/B20 uses WREN.0/.1 and TOP_W_SEL/BOT_W_SEL=0/0;
A20/B10 uses WREN.1/.0 and selectors 1/1. Both retain CLK1/2→CLKIN.0/.1,
DATAAIN/DATABIN, and DATAAOUT/DATABOUT. Addresses begin at bit 12 minus that
port's address width. Narrow data drives both halves of its input bank.
Tagged equal widths retain the existing equal-width selectors.

The [Quartus oracles](oracle/tdp-mixed) retain both 17.0.2 Lite source projects,
compressed RBF artifacts and decoded settings with pin-correlated routes.
`memory_libmap` can exchange physical A and B; tests identify the logical
ports through their enable connections, and raw primitive cases force each
physical orientation.

## Reproduce

```sh
python3 mistral/tests/m10k/true_dual_port_mixed.py \
  --yosys /path/to/paired-yosys/bin/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/misteross/boards/de10nano/pins.qsf \
  --sdc /path/to/misteross/boards/de10nano/clocks.sdc \
  --output /tmp/m10k-tdp-mixed
```

Twelve cases cover native/padded widths in both directions, 50/25 MHz and
shared 50 MHz clocks, equal widths, and raw physical orientations. Checks
require one M10K and one HPS GP interface, compressed RBF output, timing on
both ports, all six control routes, and oracle configuration settings.
Eight malformed configurations must fail. The paired Yosys tests prove
bounded RTL-to-primitive equivalence on initialized storage windows.

Generate a target probe for the matching `u10-a2-b1-c0` image with:

```sh
python3 mistral/tests/m10k/true_dual_port_mixed_probe.py \
  --unit 10 --a-lanes 2 --b-lanes 1 > /tmp/tdp-mixed-probe.sh
```

Load the image and run the probe under an exclusive kit session using the
kit's normal stop/reboot/release protocol. The same probe applies to raw
cases. It checks initialization, both write directions, NEW_DATA,
cross-width readback, preserved adjacent lanes, disjoint simultaneous writes,
and enable hold/write suppression. Address/data settle before WE rises;
WE falls before either changes. Port selection changes separately while
preserving the departing port's address/data, before staging the new port's
payload. This prevents asynchronous selector skew from overwriting a staged
address. Run `python3 mistral/tests/m10k/true_dual_port_mixed_probe_test.py`
for the host protocol regression, which includes a late-selector model and
the missed-write counterexample. Host checks alone do not establish silicon
behavior.

Exact native/padded independent-clock kit artifacts and results are retained
in [hardware acceptance](acceptance/true-dual-port-mixed/README.md).
