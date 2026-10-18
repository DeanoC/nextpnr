# BEL-locked LAB legalisation

One LAB holds two shapes that used to skip legalisation together.

`seal_pcm_bits_678` is the seal's right-PCM bits 6, 7, and 8: a bottom
2-input LUT beside a BEL-locked flip-flop. Legalisation gives its last
input F1 and its first input E1.

`plug_rdata_ff_10` is seed 6's constant 512: a BEL-locked flip-flop whose
ALM has no LUT. Its constant 0 enters through a data route-through on F1.

The flip-flop stays on the requested BEL at user strength. Enable is
tied high. Async clear, sync clear, and sync load stay unconnected.

The same routed JSON is loaded again with `--fes-scaffold` and without
`--no-route`. That is the shell half of a cart route: the restored pin
map, LUT mask, flip-flop DATAIN net, and route-through stay as written.
A pin map that a fresh reassignment would replace also stays.
