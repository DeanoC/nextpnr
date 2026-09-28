# Ready1 shortest-path diagnostic

`NEXTPNR_MISTRAL_READY_FALLBACK=<prefix>` with
`NEXTPNR_MISTRAL_READY_SHORTEST=1` replaces the existing final diagnostic's
GPU candidate generation with one exact Dijkstra candidate (variant 200).
Without the prefix the mode is ignored before identifier allocation. An absent
or empty shortest flag retains the existing GPU candidate path.

The search minimizes the final frozen calibrated scalar pip delay plus wire
delay. It uses the current occupied device graph, permits resources belonging
to the target, excludes other nets' wires and unavailable pips, and rejects FES
fence contexts where availability can depend on target binding. Every unique
downhill edge of each expanded node is exported, including blocked edges and
raw/unique degrees. Queue ties use numeric wire ID, adjacency is sorted, and
only strict distance improvements change predecessors. The search stops when
the sink is popped. The node/edge certificate supports replay and a capped
potential shortest-distance proof; physical adjacency completeness can also
be checked against the device database.

Exactness concerns this fixed graph and scalar objective. It does not prove
minimum setup delay or optimality among routes passing the endpoint guards.
The single candidate still must bind, preserve the complete baseline placement
check vector and unrelated routing/model state, and pass the existing 224
endpoint setup/hold and all-clock guards. Failed analogue results remain failed.
The original path bounds the certified distance; a bound candidate's Context
scalar must equal that distance. A rejected candidate restores the original.

`ReadyShortestTest.*` exercises the shared search engine (weighted optimum,
blocked shortcut, zero cycles, deterministic ties, unreachable sink, negative
and overflowing costs), actual calibrated backend resource exclusion and route
binding/restoration, and disabled-prefix identifier neutrality. Its generated
wire graph is a backend API fixture, not a physical FPGA acceptance test.
