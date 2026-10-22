# Experimental private two-LUT decomposition

`--remap-private-pair-critical REPORT` lists predicted candidates after fresh
ordinary HeAP placement. Add `--remap-private-pair-candidate N` to select exactly
one qualified candidate; a missing or rejected index fails before routing.
Listing requires `--no-route` and no RBF output. Saved JSON settings never enable
this pass. It cannot combine with another placement rewrite; pre-placement
reduction balancing and the ordinary placer configuration remain available.

The initial scope is one private ALUT3 output feeding one ALUT5 input, with seven
distinct essential external signals each used once. There are 35 disjoint
three-bound/four-free partitions and two complementary internal encodings per
partition. Every candidate is recomposed over all 128 external assignments.
There is no shared-input, LUT6, duplication, constant-folding or broader cut
extension in this experiment.

Both cells, output nets, BELs, strengths, attributes and all eight existing input
user slots are preserved. The transaction reassigns slots directly without
adding cells, nets, aliases or route-throughs. Native LAB legality, input maps,
mode bits and physical LUT masks are checked. Stationary ALM mates must retain
their exact native pin maps and emitted mask bits. Rejection and exceptions
restore the ports, parameters, user stores, LAB state, architecture state and
bindings. Protected cells, constraints, clusters, regions and routed contexts
are ineligible.

Arrival ranking requires seven characterized direct FF/M10K registered outputs
with complete native window coverage and one common launch clock/edge. Every
output branch must reach a fully modeled FF/M10K data capture through ordinary
LUTs. Unknown, cut, mixed-frame, hybrid, cyclic or over-bound cones reject. At
most four distinct partitions per reported pair receive native trials, with at
most sixteen reported pairs. Predicted arrival ranking only chooses trial order.

The existing complete endpoint guard determines predicted qualification using
fresh skewed and reference-free analyses. It preserves every known setup and
hold margin, endpoint/domain/window identity, and all clock, IO, unsupported and
physical contracts, and requires at least 20 ps gain on an existing setup
failure. No timing exceptions, new clock assumptions, or coverage waivers are
introduced. Unchanged unsupported coverage remains unsupported.

This is an opt-in compiler experiment. A listed or selected placement is not
routed timing, a production seal, or hardware acceptance. Qualification requires
the complete fresh routed artifact and its existing acceptance gates.
