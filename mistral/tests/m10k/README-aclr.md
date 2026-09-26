# M10K asynchronous clear controls

The Mistral M10K BEL exposes the two physical clear inputs as logical
`ACLR0` and `ACLR1`, mapped to `ACLR[0]` and `ACLR[1]`. Packing materialises
both inputs even when the source primitive omits them. An omitted input and a
constant zero are retained as `PIN_0` and leave the corresponding output clear
disabled. A constant one stays disconnected as `PIN_1`, using the M10K
default-high input. A signal or an inverted signal remains a routed
clear net; an inverted signal also selects the clear inverter.

The M10K output-clear path is enabled only for a live clear input. `ACLR0`
selects the top output-clear source and `ACLR1` selects the bottom source;
enabled outputs use the corresponding `*_OUTCLR_EN=REG` and
`*_OUTPUT_SEL=REG` settings. Address-clear enables are deliberately not
programmed: Cyclone V ignores address clear when the input address registers
used by these primitives are enabled.

The host regression covers omitted controls, hard zero/one constants, two
fabric-driven controls, both SDP and true-dual-port cells, compressed RBF
generation, decoded M10K settings, clear routes and a 50 MHz timing report.
It is a host-only check; no hardware acceptance is claimed.

```sh
python3 mistral/tests/m10k/aclr.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf mistral/tests/sdr-input/pins.qsf \
  --sdc mistral/tests/sdr-input/clocks.sdc \
  --output /tmp/m10k-aclr
```
