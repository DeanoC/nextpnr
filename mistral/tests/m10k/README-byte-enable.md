# M10K 20-bit byte enables

The byte-enabled slice maps a 512x20 simple dual-port RAM to one Cyclone V
M10K. Its two logical write-mask bits route to `BYTEENABLEA[0:1]`; the logical
write enable routes to positive `WREN[0]` and the matching write core enable.
The read side retains the independent `CLKIN[1]` path from the dual-clock
support. Other M10K widths keep the legacy write-enable mapping.

Run the host fixture with the paired Yosys installation and this nextpnr build:

```sh
python3 mistral/tests/m10k/byte_enable.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/pins.qsf --sdc /path/to/clocks.sdc \
  --output /tmp/m10k-byte-enable
```

The test checks one M10K, one PLL and one HPS GP interface, compressed RBF
generation, the byte-enable and write-control routes, the 20-bit selector
settings, and 50 MHz timing. The companion `byte_enable_probe.sh` runs on the
designated DE10-Nano under a `kit.py` lease and checks initialized reads,
independent low/high lane writes, a zero mask, and a full write while the read
clock is stopped. The probe uses GPI signature `D612`; it is a hardware
acceptance check for this fixture, not a claim about collision semantics or
true dual-port operation.
