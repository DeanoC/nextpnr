# Report-guided control decomposition

The optional `--remap-decompose-critical FILE` stage runs after fresh ordinary
HeAP placement and earlier selected remaps. It uses a validated timing report
from the same netlist. It is disabled by default and is not serialized in JSON.
Report names identify connections; no particular RTL names, state encodings,
constants or coordinates select a rewrite.

Listing requires `--no-route`, forbids `--rbf`, and restores the original netlist
and placements. `--remap-decompose-candidate N` applies the Nth qualified trial.
An unqualified selected candidate stops before routing, including with `--force`.
Earlier local, comb and placed reduction listings cannot precede this stage.
Reloaded packed/placed/routed designs and non-HeAP placers are rejected.

The bounded fanin search includes branches beside a reported critical path.
Each cut contains exactly four ordinary LUTs and at most seven raw inputs. Its
complete 128-row function must have seven essential inputs. Every four-bound,
three-free partition with two to four cofactor classes is considered, including
injective binary encodings. Encoder and root supports are pruned; each LUT must
have two to six inputs. Every proposal is recomposed against all 128 rows.
Unused generated codes have deterministic zero outputs. No assumption about
reachable RTL states contributes to the equivalence proof.

All original LUTs, FFs and BEL assignments remain. New LUTs use ALMs without
original LUT, FF or routing-buffer occupants; new LUTs may share with each
other. Accepted rewrites keep original cells and nets as the exact iteration
prefix, preserving their GPU net IDs; new clones follow them. New loads and
queue sizes can still change routing. One selected LUT input
receives an independent replacement root driven by one or two encoders. Other
consumers retain the original cone. Protected cells/nets/LABs, regions,
global/clock/top-level roles, unsupported timing and downstream cycles exclude
a candidate.

At most 16 cuts and four different support patterns per cut are probed. Source
arrivals include upstream data arcs and clock-to-Q once. They rank pin orders
and joint placements; full timing analysis decides eligibility. Shortlists keep
at most eight sites per LUT and two per LAB. The timing budget is four legal
joint probes per support pattern and 16 per cut, with at most two ordered
encoder arrangements per support pattern for the same root/encoder LAB multiset.
Illegal placements
consume no timing budget. Every bound neighbour in affected LABs must remain
legal. Rejected transactions restore owner dictionaries, indexed net-user
stores and original connections.

A trial must improve the selected input setup slack by at least 250 ps, preserve
all downstream endpoint and characterized hard-block boundary setup slacks,
preserve achieved Fmax on every clock, and introduce no new or worse reported
hold violations. This placement-model gate still needs full routing, complete
truth/connection/physical-pin isolation checks, all-clock comparison and final
hold analysis before a performance gain can be claimed.
