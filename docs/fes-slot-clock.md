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
