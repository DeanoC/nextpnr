# M10K address-stall controls

`MISTRAL_M10K` exposes the Cyclone V M10K address-stall inputs as
`ADDRSTALLA` and `ADDRSTALLB`.  They are dedicated GOUT control pins; packing
maps a connected logical port directly to the matching BEL pin and does not
change the RAM mode fields.  Omitted ports retain the device's default low
state.  A constant or fabric-driven port is kept in the packed netlist so the
constant or signal can reach the physical control.

The [Quartus 17.0.2 oracle](oracle/address-stall) for a true-dual-port M10K
routes both controls to the M10K GOUT resources and emits no additional
M10K selector field. The host regression uses an independently clocked
TDP fixture, two independent HPS GP fabric nets, and a second case with
constant zero/one controls.  It checks one packed M10K and one HPS GP cell in
each case, and verifies both decompiled routes plus a 50 MHz timing report and
compressed RBF.

```sh
python3 mistral/tests/m10k/address_stall.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/misteross/boards/de10nano/pins.qsf \
  --sdc /path/to/misteross/boards/de10nano/clocks.sdc \
  --output /tmp/m10k-address-stall
```

The fixture is host-only.  It establishes packing and routing of the
controls; it does not claim a hardware address-hold acceptance test.
