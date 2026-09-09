# Byte write enables for true dual-port M10K

Use the paired Yosys `ram_style="m10k_tdp_byte"` style for two masked
read/write ports in one Cyclone V M10K. Each port has a clock enable, master
write enable and two active-high byte masks. Supported payloads are 512×20
(two 10-bit lanes) and 512×16 (two 8-bit lanes padded individually to 10 bits).
Both ports have the same width. The ordinary `m10k_tdp` style retains its
existing whole-word write-through semantics.

Use the inference idiom in `true_dual_port_byte.v`: on an enabled edge, write
selected byte slices when write enable is high; otherwise assign memory to
the read output. The RTL holds that output during writes, and Yosys adds any
needed hold logic to preserve this behavior. Do not infer guaranteed readback
of disabled bytes on the same edge as a write.

Only enabled bytes are written. A zero mask preserves the whole word; a
low/high mask preserves the other byte. At the standalone primitive interface during a write, enabled output bytes
return new data and disabled output bytes are unspecified. A later read
returns the full stored word, including preserved bytes. Clock enable low
holds the output and suppresses writes. Cross-port accesses to the same word
involving a write remain undefined; even disjoint masks do not promise a
same-word collision guarantee. There is no reset or mixed-width mode.

The existing `MISTRAL_M10K_TDP` primitive adds `CFG_BYTE_ENABLE=1`,
`A1BE[1:0]` and `B1BE[1:0]`. Byte mode requires `CFG_ABITS=9`, `CFG_DBITS=20`
and two connected mask bits per port. The default flag remains zero for old
JSON. nextpnr maps the masks to `BYTEENABLEA[0:1]` and `BYTEENABLEB[0:1]`,
respectively, including routed constant zeros and ones. Each mask is timed
against its own port clock using the existing M10K write-control setup/hold
values. No Mistral tables or bitstream selector changes are needed; the
retained [Quartus oracle](oracle/tdp20-byte) confirms the mapping.

```sh
python3 mistral/tests/m10k/true_dual_port_byte.py \
  --yosys /path/to/paired-yosys/bin/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/misteross/boards/de10nano/pins.qsf \
  --sdc /path/to/misteross/boards/de10nano/clocks.sdc \
  --output /tmp/m10k-tdp-byte
```

The host fixture checks both payload widths with independent 50/25 MHz and
shared 50 MHz clocks, plus a direct 20-bit primitive case, one M10K, mask/control routes, decoded settings,
compressed RBF output and clock timing. Invalid geometry, mixed mode and
missing clocks/masks or extra mask bits must fail. A separate route case checks
complementary constant-zero/one mask bits on both ports. The companion Yosys regression checks masked
storage and defined output behavior with bounded SAT comparisons.

Generate a target probe with:

```sh
python3 mistral/tests/m10k/true_dual_port_byte_probe.py --width 20 > /tmp/tdp-byte-probe.sh
```

Under an exclusive kit session, load the matching `w20-c0/top.rbf` and run the
probe on the target. It checks initialization, low/high/zero/full masks on
both writers, output hold during writes, preserved bytes on later reads,
simultaneous disjoint writes with different masks, and clock-enable hold.
Use width 16 for the padded-byte fixture. The probe settles addresses/data/masks
before asserting write enable and deasserts it before changing them again.
Only the board's 50 MHz reference is needed. These settled-data checks do not
establish CDC timing, stopped-clock behavior or collision semantics.

The `w20-c0-raw` case bypasses inference and checks the standalone primitive.
Use `--width 20 --raw` when generating its probe: enabled bytes must return
NEW_DATA during a write; disabled bytes are excluded from that write-output
comparison and are verified on subsequent reads. Do not use the inferred
output-hold probe with this raw image, or the raw probe with an inferred image.

Exact hardware-tested RBFs, hashes and results are retained in
[hardware acceptance](acceptance/true-dual-port-byte/README.md). The bundle
distinguishes inferred-output and raw-output probes.
