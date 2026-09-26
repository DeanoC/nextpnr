# FES static socket clock

FES cart merge (`--fes-scaffold --fes-cart cart.json`) consumes a previously
packed/routed shell. A cart is independently synthesized and routed against
that frozen shell; runtime CRAM linking performs no place-and-route.

For a shell with multiple clock domains, supply the exact socket clock net:

```
nextpnr-mistral --json shell-routed.json --fes-scaffold --fes-cart cart.json \
  --fes-slot-clock clk_sys --device 5CSEBA6U23I7 ...
```

The selected name may be a net alias but must resolve to a driven shell net.
An absent or cart-driven net is rejected. Without this argument, the original
single-clock diagnostic remains supported: all non-slot shell flip-flops must
share one clock. An ambiguous shell now fails instead of choosing whichever
flip-flop happens to be visited first. A shell without flip-flops may fall back
to its driven `FPGA_CLK1_50` net.

The socket has one clock. Cart flip-flops, simple-dual-port M10K RAM, and both
ports of true-dual-port M10K RAM use that clock after cart-only synthesis
buffers are removed. Independent cart clock domains are not supported.
Explicit constant inputs retain their values through merge and the existing
unbound constant packer; write enables cannot silently inherit pin defaults.

The focused regression synthesizes a dual-clock shell and a cart with SDP RAM,
writable TDP RAM and a flip-flop, runs real merge/packing, and checks the chosen
clock and constant controls. It also covers missing clocks, ambiguous implicit
selection and the original single-clock fallback:

```
python3 mistral/tests/fes_slot_clock.py --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral --output /tmp/fes-slot-clock
```

This regression validates merge and packing. It does not claim timing closure,
CRAM overlay equivalence or hardware acceptance for a consumer shell.

The merge regression also routes a shell containing RAM before importing the
cart. Imported physical BEL pins are used when detaching a vacant return sink;
logical pin maps are not yet restored at that point. Cart packing skips bound
shell M10Ks, preserving their already packed control modes. The regression
compares retained shell cell parameters, BEL attributes, connections through
stable net aliases, and shared constant routing before and after merge.

Routed JSON now carries each placed cell's `FES_PINMAP_V1` attribute: canonical
JSON encoded as lowercase hexadecimal, with an explicit entry count and a
`pins` object mapping logical names to arrays containing the folded pin state
and physical BEL pin names. The scaffold restores these exact maps, including
unused RAM lanes and hard constants that cannot be inferred from routed nets.
It rejects malformed, incomplete or unsupported metadata before placement.
Physical pins are checked against their logical direction, including legitimate
bidirectional I/O pads.

The module's `FES_LABSTATE_V1` attribute uses the same encoding for the device
identity and ordered LAB geometry. Each LAB preserves its two clear-use flags
and ten ALMs' LUT6/carry modes, clock/enable selectors and clear selectors.
Counts, coordinates and selector ranges must match the target device exactly.
Cart cells cannot share a LAB with frozen shell cells because those modes and
control selectors are shared hardware. A routed shell without either snapshot
must be rebuilt; the packed, non-scaffold merge path remains available.

The routed regression also restores the shell as a locked scaffold, places and
routes the cart, and checks frozen BEL and pin-map preservation. A shell with
LUT6 logic and an inverted FF enable must emit byte-identical RBF when restored
without a cart. These compiler
checks do not replace a consumer's CRAM-boundary and timing checks.

`--fes-cram-region x0,y0,x1,y1` fences new routing in an already routed
scaffold by the actual routing mux configuration bits, with exclusive upper
bounds. It requires a Mistral library exposing `rnode_mux_cram_bits`; older
libraries reject the option. Bounds must fit the device CRAM geometry. Tile
coordinates alone are insufficient because long-wire muxes may be programmed
outside their nominal destination tile.

The original scaffold's exact pip selections remain available. Every other
physical mux must have its entire footprint inside the supplied region,
including additions to shared shell/cart ground and supply nets. Synthetic BEL
edges are skipped exactly as in bitstream routing emission; they have no routing
mux bits, and their cell configuration remains governed by the placement fence
and frozen physical snapshots. Before emitting RBF, the compiler checks all
routed pips again. Consumers must still compare the complete emitted CRAM and
header against their sealed shell; this routing check does not cover every
possible cell or device setting.

For a cart with a physical CRAM region, unbound packing gives remaining constant
consumers local slot `MISTRAL_CONST` drivers after control folding and RAM setup.
The frozen shell's constant drivers, users and routes remain intact. This avoids
depending on new branches from a distant shell constant tree outside the region.

The regression replays the unchanged shell with a one-bit region and requires
identical bytes, proving existing outside routing is retained. Invalid bounds
and use without a routed scaffold must reject before compilation.
