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

Adding `NEXTPNR_MISTRAL_READY_RELAXED=1` selects a read-only occupancy-relaxed
search (variant 201). It requires shortest mode and the fallback prefix. Only
foreign wire/pip ownership is ignored; reserved-route and CRAM restrictions
remain enforced. The certificate retains actual owners, and a separate
`shortest-blockers.tsv` lists foreign resources on the hypothetical path.

This mode never binds the hypothetical path, rebinds the original, or triggers
bitstream reconfiguration. The full route/cache/placement state and final RBF
must remain identical, as must all 224 endpoint setup/hold values and every
clock's Fmax. Its distance is a lower bound for a less constrained routing graph,
not a routable implementation or a measured timing improvement. The backend
fixture verifies a foreign wire/pip shortcut while a cheaper statically reserved
alternative remains unavailable; the ordinary fixed-occupancy search is checked
in the same fixture.
