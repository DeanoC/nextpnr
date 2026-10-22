# Experimental report-guided LUT swaps

`--critical-swap-budget N --critical-cohort-report baseline.json` enables a
bounded placement-only repair on an unrouted placed checkpoint. Use
`--no-pack --no-place`; `N` is 0..64 complete STA trials, default 0. Saved JSON
settings cannot enable it. The report must be a final analogue timing report
with matching clocks and graph hops, using the existing cohort report parser.
Do not combine with cohort placement repair or frozen FES slot/scaffold modes.

The pass extends the critical-cohort transaction and timing guards with direct
occupied-site exchanges. It considers ordinary unclustered LUTs on both ends of
the longest reported critical hops (prioritizing carry-to-LUT hops), including result muxes driven by carry chains. Carry
cells, registers, constraints, functions, nets, port states and physical pin maps
remain unchanged. Fixed, kept, region-constrained and clustered LUTs are excluded.
An occupied destination must itself be movable. Every occupied BEL in both
affected LABs must pass existing legality checks. No input-budget or mixed-mode
packing rule is relaxed. Rejected and throwing trials restore bindings, strengths
and LAB state.

Each iteration considers at most eight LUTs, sites within Manhattan radius three
of the opposite hop end, and 32 legal ranked sites per LUT (at most four per tile), checking at most
256 geometries per source before STA. Ranking sums every incoming
and outgoing data connection of both exchange participants with the existing
critical-cohort weighting. Trials visit different LUTs in rounds. Across the
whole invocation, geometry attempts are capped at 128 times the STA budget.
Accepted moves trigger fresh timing and proposals; exhaustion stops the pass.

Qualification uses native ideal-clock placement STA with routing's reversible
physical-pin and FF route-through preview. One endpoint on a reported failing clock must
gain at least 20 ps, while worst slack and every clock Fmax remain nonregressing.
The existing cohort guard protects all available endpoint clock-pair rows:
failing setup margins cannot worsen, passing endpoints must stay passing,
unrelated early/late path extrema cannot regress, and related hold violations
cannot be introduced or worsened. This is predicted timing only. It does not
promise that every positive setup or hold margin increases, or that complete
routing will preserve the gain. Baseline and candidate must still be compared
with full final analogue setup/hold evidence before adoption.

This is an opt-in experiment for nextpnr #265, not a PC/XT timing-closure or
hardware-acceptance claim. It never changes clocks, pipelines or timing gates.

## Full-route retention guard

Use `python3 mistral/routed_swap.py --help` for an isolated paired run. It copies
one compiler binary, placed checkpoint, QSF, SDC and guidance report into a new
output directory, then runs baseline and candidate from the same inputs with
full routing and final analogue STA. The driver supports the FES phase-lease
runner for GPU routes; it never holds a lease during CPU work. The default bound
is two 600-second invocations, charging all elapsed time including lease waits.
No seed search, clock relaxation or `--timing-allow-fail` is used.

`--routed-endpoints FILE` exports native setup/hold rows for every connected
registered or IO endpoint after bitstream signoff. It is independent of bounded
`--timing-report-paths`. Actual clock skew is included for setup/hold; unrelated
path extrema come from a separate unskewed analyser in that same full-route
analogue frame. Clock constraints include phases and phase groups. Unsupported
paths, unknown clocks, incomplete tags and IO paths unsupported by the internal
API are explicitly listed and make coverage incomplete. Even identical lists of
unsupported endpoints cause rejection. This conservative limitation can make the
pass a guarded no-op on real designs; it is not permission to drop those rows.

Retention requires identical complete endpoint/clock-pair inventories, clock
constraints and timed/related classification. Every setup and related hold
margin must be nonregressing, including positive margins. Unrelated extrema and
all clock frequencies must be nonregressing. At least one setup margin must gain
20 ps. The original prediction only proposes candidates; aggregate Fmax and the
intersection of bounded reports never authorize retention.

`OUTPUT/retained` atomically selects the entire `baseline` or `candidate` bundle.
On rejection, route failure or a caught exception it points to the untouched
baseline. The RBF, checkpoint with placement/routing/pin attributes, native
inventory and signoff report are selected together, without a reload or another
STA frame. Both process logs, receipts and all failed artifacts are preserved.
The decision includes selected artifact hashes and the retained process status;
a safe relative improvement does not turn a failing baseline into a qualified
100 MHz design. The driver returns the retained process exit status.

Direct full-route use of a positive swap budget now requires the explicit
`--critical-swap-routed-trial` acknowledgement and emits an **unqualified** trial.
The driver supplies this flag only in its isolated candidate directory. Ordinary
invocations and budget zero remain unchanged. Placement-only experiments still
work, but their outputs are not routed acceptance evidence. The guard is an
opt-in compiler driver, not an in-process Context rollback or a general
transaction over arbitrary user scripts and output paths.

### Static endpoint coverage

Schema 2 inventories distinguish dynamic timing rows from proven static inputs.
A direct `MISTRAL_CONST` (fully defined zero/one LUT parameter), optionally through
identity `MISTRAL_BUF` route-throughs, has no launch transition and hence no
setup/hold pair. The exporter records its source chain, value, sink parameters
and pin states, and capture clock/edge. It requires a characterized registered
sink and a constrained capture clock. The driver requires these proofs and their
coverage to be identical across the pair. Static endpoints never contribute a
zero delay, a timing gain, or a fabricated margin. Unknown constants, arbitrary
LUT cones, cycles, undriven inputs and unsupported IO/control endpoints do not
qualify for this structural classification.

PLL `locked` is an asynchronous startpoint. A path from it to FF `ACLR` still
has no supported registered-launch timing pair or recovery/removal model. Such
paths remain unsupported even if the netlist and route are unchanged. Completing
static coverage does not authorize retention when these paths are present.

Unconstrained external input paths also remain unsupported. In particular,
constant-fed and IO-fed route-through buffers must not be conflated: a
`MISTRAL_IO.O` source is not static. External launch requirements require actual
board/interface constraints and supported IO timing, not invented clock tags.

## Connected LUT pairs (experimental)

`--critical-swap-pairs` selects atomic pairs instead of the single-LUT search.
It requires an explicit positive `--critical-swap-budget` and inherits the
placed-checkpoint, routed-guidance and unqualified-route restrictions above.
Saved settings cannot enable it. Defaults and ordinary placement are unchanged.
For an isolated full-route comparison, pass `--pairs` to `mistral/routed_swap.py`;
only the candidate enables paired swaps, and retention checks are unchanged.

The search selects up to eight distinct connected ordinary-LUT pairs from long
reported hops. It considers a shared destination LAB near either source LAB,
their midpoint, or the mean location of their external data neighbours. The
radius is one tile. It ranks at most eight sites per cell per tile, including
the displaced LUTs' complete data boundaries, then tests at most 256 pair
geometries per source pair and retains up to 32 legal proposals (four per tile).
The existing global limit of 128 geometry attempts per STA-budget unit applies.
Candidates take turns across source pairs. A successful move restarts discovery.

Both LUT moves and up to two displaced occupants form one transaction: legality
is checked on the complete resulting placement, then the same full-design
clock, endpoint, related-hold and 20 ps setup-gain guards apply. Rejection or an
exception restores every participant. Source homes and target sites must be
disjoint; protected or clustered cells are excluded. Registers, carry chains,
netlist functions and connectivity remain unchanged. This can explore a compact
placement that individual moves cannot reach, but predicts no routed gain.

## Shared data LUT replication (experimental)

`--critical-swap-replicate` selects exact-function replication instead of swaps.
It requires a positive `--critical-swap-budget`, excludes `--critical-swap-pairs`,
and inherits the placed-checkpoint, authenticated guidance and unqualified-route
restrictions. Saved settings do not enable it. The isolated route driver accepts
`--replicate` on its candidate only; routed retention checks are unchanged.

The search selects up to eight shared ordinary-LUT data edges from authenticated
failing-clock guidance. It ranks empty sites within radius two of the consumer
or source/consumer midpoint, retains up to 32 proposals per edge (four per tile),
and visits edges in rounds. STA and geometry limits remain the explicit budget
and 128 times that budget. Registers, carries, protected cells and top-level
boundary nets are excluded. Inputs must be connected ordinary data signals or
explicit disconnected constants; input polarity is preserved.

A trial copies the exact LUT type, parameters, input nets and pin states and
redirects one eligible ordinary-LUT input or FF DATAIN. The original source,
other consumers and all original placements remain. The copy's function is
identical for every input combination. Full affected-LAB legality and the same
native endpoint, clock, related-hold and setup-gain guards decide retention.
Rejection and exceptions restore user slots, sink connection, net aliases and
LAB state. Accepted copies restart discovery; consumed authenticated edges end
the search cleanly. Malformed initial guidance still fails.

This is a placement prediction. It does not model every routing consequence of
extra input fanout and does not establish routed timing or physical acceptance.
A complete paired route and the existing coverage/retention gates are required
before adopting the result. No ordinary recipe or default enables replication.
