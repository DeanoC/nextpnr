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
runner for GPU routes; it never holds a lease during CPU work. The copied
executable keeps the `nextpnr-mistral` name required by that runner. Dispatch
fails before launch if the runner does not recognise the GPU command, and a
completed GPU route must have lease-grant evidence in its log. The default bound
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

## Post-route data-net repair

`--critical-route-budget N` (0..32, default 0) enables a separate post-route
experiment. It requires an ordinary complete GPU route with `--rbf`; it cannot
be enabled by loaded checkpoint settings. The retention driver selects it with
`--route-repair --budget N`, exclusively from placement swaps/pairs/replication.
Both processes first execute the same ordinary routing recipe. Only the
candidate then attempts the new pass, avoiding checkpoint-reload timing drift.

The pass proposes up to four alternate trees per selected failing ordinary
data net. For multi-sink nets, two of the four slots are reserved for whole-tree
alternatives, so single-sink diversity cannot exhaust them. This reservation is
explicit at this call site; ordinary candidate-router callers keep their policy.
Before full-design trials, each proposal is temporarily bound and the native
analogue model measures every sink, including branch loading and terminal LUT
input delay. The minimum predicted sink slack ranks candidates; only proposals
with at least 20 ps worst-sink gain proceed. This is a conservative prefilter,
not a timing proof: full endpoint and hold checks still decide retention.
One additional bounded generation feeds the largest measured per-pip delays
from rejected proposals back into the search model. Exact wire/pip duplicates
are skipped across generations. This permits at most eight native proposal
measurements per selected net, with the original budget still limiting full
STA trials. Learned costs never become timing evidence: the original fallback
table is restored before evaluation and before final signoff. Ranking always
restores the original physical tree. Original cell placement,
functions and net connectivity stay fixed;
other nets stay bound and obstruct candidate routing. A trial verifies a rooted,
acyclic tree reaching every original sink before applying it. Rejection and
exceptions restore the net's original wires/strengths, physical mux state and
analogue cache. GPU candidate generation releases its lease before CPU STA.

Each trial uses fresh full-design analogue setup/hold analysis with the original
fallback timing table. It must improve an already failing setup endpoint by at
least 20 ps, preserve every other setup and related-hold margin (including
positive margins), all clock margins/Fmax and the full endpoint/interface
inventory. Ordinary data boundaries exclude clocks, globals, external IO,
protected nets/cells, carry chains and control consumers. Search is diagnostic:
unchanged unsupported rows stay unsupported. Full artifact retention still
requires complete qualified coverage; this option never waives that gate.

## Post-route LUT moves with incident rerouting

`--critical-move-route-budget N` (0..16, default 0) enables an independent
post-route experiment. It requires an ordinary full GPU route with `--rbf` and
cannot be combined with `--critical-route-budget`. Loaded settings cannot enable
it. The retention driver selects it with `--move-route --budget N`.

The pass examines at most 32 failing ordinary LUTs, generating empty-site
proposals around the current site and at most four connected neighbors. It
retains at most four legal geometric proposals per cell, then visits cells in
criticality order round-robin. A slack-weighted Manhattan estimate orders
proposals only; it cannot authorize retention. Registers, carry cells, protected
cells, clustered cells and occupied destination sites are not moved.

Each transaction rips up only the moved LUT's incident data nets (at most seven,
with at most 64 sinks per net), preserving all other bound routes as obstructions.
A CPU reverse-Dijkstra search consumes at most 500,000 queue visits across the
whole transaction, obeying architecture pip/wire availability and existing tree
parents. There is no negotiated rip-up of other nets. Local route failure,
exhaustion, rejected timing and exceptions restore placement, LAB state, original
wire/pip strengths and physical mux/cache state together.

Only fresh native timing can accept a move: the complete endpoint/contract
inventory, all setup/related-hold margins including positive margins, all clock
margins/Fmax and at least 20 ps gain on an already failing setup endpoint are
required. Unsupported boundaries remain diagnostic and cannot pass the outer
artifact-retention gate. The graph and cell functions are unchanged; accepted
moves may change only the moved cell's placement and its incident data routes.
