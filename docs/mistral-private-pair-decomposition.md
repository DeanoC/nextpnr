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
# Separate live routed diagnostic

`--critical-private-pair-budget N` (0..4, default 0) enables a separate bounded
post-route trial. It requires a fresh ordinary full GPU route with `--rbf`,
without reload/partial/frozen execution, forced timing, or move repair. Explicit
single-net route repair and LUT6 swaps may precede it. Saved settings never
enable the pass. The existing placed private-pair API remains strictly unrouted.

Discovery uses actual negative native path witnesses, capped at 16384 paths per
clock pair and 64 producer candidates. It considers exact private ALUT3→ALUT5
seven-input decompositions, diversifies partitions before complementary
encodings, and ranks using promotion of witnessed two-level inputs and native
source arrivals. These ranks propose trials; they are not candidate STA.

Both ALMs must have ordinary ALUT2..5 occupants. Ordinary logic in LAB and MLAB
tiles is supported; LUTRAM, LUT6 and carry occupants are rejected. Cohabiting
FFs must use their local COMBOUT for DATAIN and have no SDATA. Direct native
FF/M10K sources need one characterized common launch clock and complete native
relationships. Protected, clustered, reserved, global and public boundaries
remain ineligible.

Each transaction proves all 128 input combinations and preserves the eight
original indexed net-user slots. It changes only the two LUT masks/ports/states
and their native ALM input assignment, without unbinding BELs, inserting helpers
or reassigning controls. Native mate masks/maps, FF state and every configuration
field in both affected LABs except the two changed LUT masks must remain exact.
Actual changed sink sets determine which external nets need routing. Every old
source-to-stationary-user path and its strengths is retained; a shared bounded
500000-node local search extends those rooted trees without foreign rip-up.
`--critical-private-pair-route-nodes N` explicitly changes this shared per-trial
allowance to 1..2000000 nodes and requires a positive private-pair trial budget.
The absent option and explicit 500000 use the same search/order; saved settings
never configure this allowance. This option does not alter other repair passes,
the maximum four trials, eligibility, or any setup/hold/coverage acceptance gate.
A larger allowance permits more search; it does not establish a timing gain.

Every external branch and the consumer output requires complete bounded native
LUT/BUF/arithmetic-to-FF/M10K setup/hold coverage. Fresh analyses with and without
clock skew, all-clock guards and the complete endpoint/contract guard require no
setup or hold regression, including positive margins, and at least one failing
setup endpoint gaining 20 ps. Changed clock metrics and exact rejection reasons
are logged. Rejection or exception restores ports, masks, indexed stores, native
architecture state, route parents/strengths and configuration/timing caches.

The final checkpoint refresh includes this pass. Unchanged unsupported rows
remain unsupported; an accepted local trial is not whole-board, IO or hardware
qualification. The no-option and explicit-zero paths do not run discovery,
mutation or checkpoint refresh for this feature.
