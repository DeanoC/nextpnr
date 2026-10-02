# Experimental registered hard-input locality

This default-off placement diagnostic moves an existing ordinary FF closer to
its directly connected registered input on a fixed hard block. It preserves
the full graph, register parameters and initial value, connected enables and
other controls, every pipeline stage, and all other cell placements. Discovery
uses native timing classes and connectivity; it does not match design names,
particular hard-block pins or saved coordinates.

```sh
NEXTPNR_MISTRAL_HARD_INPUT_LOCALITY="current-placed-timing.json 8 24" nextpnr-mistral ...
```

The three fields are a current pre-route guide, an attempted unique-FF budget
from 1 through 64, and a Manhattan radius from 1 through 24. Request bytes are
read before packing and are never restored from serialized settings. The pass
runs after the existing LUT pair and LUT driver-copy stages, preserving their
accepted graph and placement prefix. Generate a new placed guide from the same
input and exact selected prefix; a routed report with inserted BUF owners is
not normalized. All exported guide paths, including currently passing paths,
must have current owners, ports, BEL locations, raw net edges and native clock
events before any FF trial.

Only fresh, ordinary, successful HeAP placement can enable the pass. Packing
or placement bypasses, processed input, FES slot placement, the SA placer and
single-register capture locality are rejected, including with `--force`.
Earlier local, comb, reduction, decomposition, post-plan, LUT pair and driver
listings must be final and cannot precede this pass. A selected earlier stage
which fails qualification stops before this worker or routing. The existing
capture-pipeline pass may precede it when the new guide matches that prefix.

The launch FF must be weakly bound, unconstrained, outside protected/carry/MLAB
LABs, with the complete ordinary FF port schema. Connected DATAIN, ENA, ACLR,
SCLR, SLOAD and SDATA inputs are allowed and guarded. Clock, pin polarity and
parameters stay exact. Top-boundary, routed, constant, global-data, protected,
unknown or unplaced branches do not qualify. The hard sink must be fixed by a
unique native BEL or a locked binding, and expose a finite registered input
clock arc and one actual routing wire.

Candidate ranking uses the actual hard sink wire returned by the native net
API. Hard BEL display coordinates can differ from those routing-node
coordinates. Each attempt retains up to 64 diverse geometries across 32 LABs,
using empty compatible FF halves with empty partner FF and LUT sites. At most
64 geometric preflights and 16 complete timing trials run per attempted FF.
Native legality and snapshot-preservation failures have separate counters and
do not spend a timing trial. Logs include the actual sink wire, attempted
geometry, legality and preservation rejects, timed trials and retained count;
only the first four preflight failures are detailed.

Every retained move must improve both the native hard-input setup slack and
the true-wire delay estimate by at least 250 ps. A bounded walk covers every
Q branch, registered feedback endpoint, connected incoming data/control
endpoint and the other branches of each incoming net (at most 1,024 nets,
4,096 edges and 2,048 registered endpoints). Unsupported branches, pure
combinational cycles or incomplete clock-pair timing reject the attempt.
All covered native setup margins and maximum delays must not regress. There
is no passing-margin or internal-cycle waiver for incoming paths or feedback.
Related holds remain passing, or do not worsen an existing failure. Unrelated
clock-pair arrivals are also compared without clock skew. Global clock Fmax
and constraints must not regress, and no new or worse hold failure is allowed.

All occupied BELs in all ten ALMs of both affected LABs must be native legal.
The transaction independently preserves cell and net owners, insertion order,
ordered ports, raw indexed users and allocation order, aliases, parameters,
attributes, BEL strengths, pin maps, clock constraints and all unaffected
architecture/LAB caches. Rejection or an exception restores only the moved
FF's original binding and every saved cache. It never unbinds the fixed hard
sink, and preserves edits accepted by earlier stages.

The placement predictor still uses hard BEL display coordinates. The
additional real-wire ranking and gain test do not alter that delay model.
A candidate attractive to the physical estimate can therefore be rejected
by the conservative native timing guard. Placement qualification is not
routed timing acceptance or hardware acceptance: a retained result still
needs a completed full route, clock/hold signoff and independent graph and
physical proof. This experiment changes no default build recipe.

Native fixtures exercise a real fixed HPS input with misleading display
coordinates, a currently passing guide, connected controls, registered
feedback and secondary Q consumers, strict incoming DATAIN/ENA rejection,
state preservation after an earlier edit, protected placements, immutable
preloaded requests after the guide file changes, and an exception during a real
provisional binding. CLI fixtures exercise default-off behavior, pre-pack
request validation, placement-mode guards, earlier listing and failure
ordering, and reload behavior. Fixture geometry is discovered from
real native sites; a missing qualifying geometry fails the positive test
instead of being treated as acceptance.
