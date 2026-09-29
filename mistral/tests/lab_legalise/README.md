# BEL-locked LAB legalisation

One LAB holds two shapes that used to skip legalisation together.

`seal_pcm_bits_678` is the seal's right-PCM bits 6, 7, and 8: a bottom
2-input LUT beside a BEL-locked flip-flop. Comb pinmap reads E0/F0. The
bottom half has to read the pins that carry the nets, D and E1.

`plug_rdata_ff_10` is seed 6's constant 512: a BEL-locked flip-flop whose
ALM has no LUT. Its constant 0 has to enter through a data route-through
on D. Leaving route-through skipped puts that constant on fabric F1.

The flip-flop stays on the requested BEL at user strength. Enable is
tied high. Async clear, sync clear, and sync load stay unconnected.

The same routed JSON is loaded again with `--fes-scaffold` and without
`--no-route`. That is the shell half of a cart route: the restored pin
map, LUT mask, flip-flop DATAIN net, and route-through stay as written.
A pin map that a fresh reassignment would replace also stays.
