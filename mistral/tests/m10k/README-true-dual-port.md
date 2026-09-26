# True dual-port M10K

`MISTRAL_M10K_TDP` implements two independently enabled, positive-edge
read/write ports in one existing M10K BEL. Packing converts the primitive to
`MISTRAL_M10K` with `CFG_TDP=1`; no Mistral tables or additional BELs are needed.
Supported physical geometries are 1024×10 and 512×20, including padded 8/16-bit
payloads. This style uses the same width on both ports. For different widths, see
[mixed-width TDP](README-true-dual-port-mixed.md). For optional
byte masks and their different write-output contract, see
[byte-masked TDP](README-true-dual-port-byte.md).

Use the paired Yosys fork's explicit `ram_style="m10k_tdp"` inference style.
Each port must describe synchronous, enabled reads and write-through: on a
write its output receives the new input data. The enable gates both reading
and writing. There is no reset. Cross-port accesses to the same word involving
at least one write are undefined, including when clocks are shared. There is
no priority between writers. Keep such accesses separated in the design;
ordinary RTL scheduling does not model the hardware collision window.

The JSON contract can be made explicit with `CFG_RDW_MODE_A` and
`CFG_RDW_MODE_B` (`NEW_DATA_NO_NBE_READ` by default) and
`CFG_RDW_MODE_MIXED` (`DONT_CARE` by default). See
[read-during-write contracts](README-read-during-write.md). These settings do
not add a Mistral table field: Cyclone V has no independent collision mux, and
Quartus emits the same physical write-through configuration for the accepted
contracts. Unsupported `OLD_DATA` and `NEW_DATA_WITH_NBE_READ` requests fail
packing instead of being dropped.

## Physical mapping

| Logical signal | Mistral port |
| --- | --- |
| CLK1 / CLK2 | CLKIN.0 / CLKIN.1 |
| A1EN / B1EN | ENABLE.1 / ENABLE.0 |
| A1WE / B1WE | WREN.0 / WREN.1 |
| A1ADDR / B1ADDR | ADDRA / ADDRB, starting at bit `12-CFG_ABITS` |
| A1DATA / B1DATA | DATAAIN / DATABIN |
| A1Q / B1Q | DATAAOUT / DATABOUT |

For 10-bit words each input bit drives both halves of its own 20-bit input
bank. Output bits use the low half. Configuration enables TRUE_DUAL_PORT and
TOP_INCLK_SEL alongside the existing independent-clock selectors, with
write-through data flow on both ports. Initialization uses the existing M10K
bit permutation. Timing assigns each port's address, data, enable, write
enable and output to its own clock, reusing the backend's M10K timing values.

## Reproduce the host checks

```sh
python3 mistral/tests/m10k/true_dual_port.py \
  --yosys /path/to/paired-yosys/bin/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/misteross/boards/de10nano/pins.qsf \
  --sdc /path/to/misteross/boards/de10nano/clocks.sdc \
  --output /tmp/m10k-tdp
```

The fixture covers native 10/20 and padded 8/16-bit widths with independent
50/25 MHz clocks, plus native widths sharing the 50 MHz reference. It checks
one M10K, one HPS GP interface, compressed RBF generation, timing, both sets
of control routes and decoded mode selectors. Invalid widths, address widths,
combined modes and a missing clock must fail with an actionable diagnostic.
Yosys's companion SAT tests check the functional mapping. Host checks do not
establish silicon behavior; run the probe under an exclusive kit session for
hardware acceptance.

Generate the corresponding target shell probe with:

```sh
python3 mistral/tests/m10k/true_dual_port_probe.py --width 20 > /tmp/tdp-probe.sh
```

Load the matching `w20-c0/top.rbf` or `w20-c1/top.rbf` and run the script through
the designated kit's existing session protocol. Substitute 8, 10 or 16 for
other payload widths. The probe checks initialization, each writer and its
write-through output, opposite-port readback, simultaneous disjoint writes,
and output/write suppression while disabled. Address and data settle before
write enable rises; write enable falls before the next address change.

Native 10/20-bit independent-clock kit results and the exact tested RBFs are
retained in [hardware acceptance](acceptance/true-dual-port/README.md).
