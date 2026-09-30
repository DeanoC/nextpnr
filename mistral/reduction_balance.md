# Bounded conjunction balancing diagnostics

The opt-in `--balance-reduction-root NAME` recognizes three LUTs with eleven
distinct literals, four LUTs with sixteen, or seven LUTs with twenty-four.
Every LUT must have exactly one input assignment for the polarity required
by its parent. Recognition examines at most 64 rows per LUT and propagates
pin inversion; it rejects repeated inputs, reconvergence, cycles and general
Boolean functions. No RTL name, constant or location identifies a candidate.

The 24-literal tree becomes four six-input product LUTs and a four-input root.
Four original private intermediates are reused in deterministic name order;
two surplus private cells and their output nets are removed. The public root
object, output net and its external consumers are retained. All literal nets
keep their existing indexed consumer slots. Non-root outputs must have one
internal consumer and no top-level connection. Placement, region, clock,
global-net, protected-attribute and malformed-driver guards apply.

The experimental `NEXTPNR_MISTRAL_PLACED_REDUCTION` environment variable accepts
one to eight newline-separated `ROOT RADIUS SELECTION` lines. Radius is 1..6;
selection is a nonnegative qualified ordinal, or -1 for final-stage listing.
The whole request is parsed before any stage executes. A selected failure
aborts before routing; listing should be invoked with `--no-route`. This
diagnostic is not serialized into a design or enabled by a recipe.

Placed balancing supports the eleven- and twenty-four-literal classes. It
fixes the public root and all outside-cone cells, and searches nearby legal
sites for two or four leaves. Each leaf has at most 24 spatially diverse
sites; the 24-literal search is bounded to 24^4 tuples and sixteen legal LAB
tuples with timing analysis. The four-leaf search times at most two legal
assignments per unordered LAB multiset, preserving repeated-LAB multiplicity
and ordered assignment deduplication. This keeps permutations of one geometry
from exhausting the timing budget. Illegal assignments consume no budget.
A candidate needs at least 250 ps modeled root
branch improvement, nonregressing downstream endpoints, clocks and holds,
and live affected-LAB legality. The shared 24-literal root may have
reconvergent downstream branches; an active-branch cycle is rejected.

Surplus objects are absent from live architecture and timing scans during a
24-literal probe. Rejection or listing restores original owners, dictionary
iteration order, drivers, complete indexed user stores, ports, parameters,
pin states and placements. Acceptance permanently retires the two private
objects. The earlier three-LUT placed search remains unchanged.

Modeled qualification is not final route acceptance. Full-route clock and
hold checks and preservation of outside-cone physical pin assignments and
routing-created buffers are still required before selecting a result.
