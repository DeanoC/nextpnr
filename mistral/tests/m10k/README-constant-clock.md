# M10K constant disabled-port clock

`constant_clock.py` covers the inferred true-dual-port shape used by write-only
RAMs: the B port enable and `CLK2` are tied low. The clock constant is folded
by the packer, so it does not become a `$PACKER_GND_NET` route to the M10K
`CLKIN[1]` TCLK sink. Bit generation retains the single-clock selector
defaults while the live A clock remains on `CLKIN[0]`.
The corresponding disabled-port enable must also be tied low; a constant clock
with an active port is rejected during packing.

The check is host-only. It synthesizes one TDP M10K, places and routes it,
writes a compressed RBF, checks 50 MHz timing, and decodes the RBF to verify
the absent second clock route and selector fields.

```sh
python3 mistral/tests/m10k/constant_clock.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf mistral/tests/pll/pins.qsf --sdc mistral/tests/pll/clocks.sdc \
  --output /tmp/m10k-constant-clock
```
