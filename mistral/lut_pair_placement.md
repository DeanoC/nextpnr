# Joint LUT placement

By default, the optional Mistral pass relocates two consecutive, existing ordinary LUTs
jointly. It keeps their truth tables, logical pins, Q nets, aliases and complete
consumer lists. It creates no cell or net and moves no FF. A terminal FF may
remain in a protected carry LAB; both source LUTs and destination LABs must be
unprotected. Each destination is a distinct, empty whole ALM with compatible
physical pins. Input pin states and their physical mapping remain unchanged.

Enable the pass with `--remap-lut-pair-critical report.json`. Omit
`--remap-lut-pair-candidate` or use `-1` to list qualified candidates. Listing
requires `--no-route`, forbids `--rbf`, and must be the final transformation.
A nonnegative candidate selects one of the freshly qualified joint moves.
An unavailable selection stops before routing, including with `--force`.
The default invocation does not run this pass.

With the explicit `--remap-lut-pair-compose-copy` option, the same stage instead
composes the final two ordinary LUTs of an FF enable path into one new LUT with
at most six distinct external inputs. This option requires the critical report.
Constants, inversions and shared inputs are included in exhaustive truth-table
evaluation. Only the complete same-clock FF enable cohort in the selected LAB
is redirected. Both original LUTs, every other consumer, all aliases and every
original cell placement remain fixed. The selected FFs must themselves be weak,
ordinary and unconstrained; protected carry neighbours in their LAB are allowed.
Source and copy destination LABs remain unprotected, and the copy occupies an
empty isolated whole ALM. Repeated copies use the first available paired cell
and private-net names without replacing an earlier copy.

Copy mode searches within the same radius three, with at most eight cones and
16 timing trials per cone, choosing distinct destination tiles. It follows all
external input fanouts and both original output fanouts for the same strict
endpoint, related-hold, reference and global-clock checks. A negative baseline
setup margin cannot worsen. No original FF or LUT is unbound, including during
a probe. Only the selected equivalent ENA control changes; other controls and
protected neighbours remain exact. Listing, rejection, an unavailable selection
and exceptions remove the new owners and restore dictionary order, indexed
consumer storage, original pin/cache records and every saved LAB field.

The report must describe the current placed graph: cell names, locations,
ports, net names and route continuity are checked before a probe changes any
binding. A violating registered ENA or DATAIN path supplies its final two LUTs
and terminal FF. The report guides discovery; the pass recomputes native
predicted timing for qualification rather than accepting its delay numbers.

The pass runs after local early plans, internal cuts, placed reduction,
decomposition and local post plans, and before LUT driver copying. Any earlier
listing is incompatible with a following pair pass. A pair listing cannot
precede driver copying. Fresh packing and ordinary HeAP placement are required;
loaded processed designs, skipped stages, simulated annealing and FES slot
placement are excluded. Reports are loaded before placement.

Search is bounded to eight distinct source-pair/terminal-port cones. It ranks
isolated whole ALMs within Manhattan distance three of the terminal, retains
at most 24 joint placements and performs at most 16 native timing trials per
cone. Every movable source and reachable side branch must have a supported
native timing boundary. Unknown clocks, unsupported fanout and feedback reject
the cone.

A candidate needs at least 250 ps of predicted target setup improvement.
Complete reachable endpoint clock-pair rows, all clock constraints and the
hold violation distribution must not regress. A setup-timed pair without
native hold coverage is rejected before movement. Unrelated clocks retain their
finite path extrema through a separate analysis without clock skew; no setup
window is invented for them. Original FF placements and controls, graph owner
order, port user slots, pin/cache records and unrelated LAB state remain fixed.
Listing, rejection, an unavailable index and an exception restore both original
LUT bindings and the saved architecture/LAB state.

`LutPairPlacementTest` uses real native LUT/FF/carry fixtures, including a fixed
protected terminal, side-user setup and related-clock hold conflicts, missing
opposite-edge hold coverage, unrelated
clock domains, occupied/protected destination LABs and stale report rejection.
Its rollback snapshot checks user-storage holes as well as pin maps, cache
records and ownership order. The exception case throws at an actual joint
trial's log boundary after binding and native timing. The portable CLI suite
checks orchestration, loading, stage ordering, default-off behavior and failure
before routing.

Qualification is predicted placement evidence. Full routing, final timing and
bitstream signoff remain required. This pass supplies no timing gain,
independent numerical STA calibration, decoded RBF equivalence or hardware
acceptance claim, and contains no RAM-specific selector.
