# Final route rediscovery after LUT repairs

`--critical-final-route-budget N` is a separate explicit diagnostic, `0..32`,
default zero. A positive value requires a positive explicit LUT6, private-pair,
or ordinary-LUT permutation budget, fresh ordinary full GPU execution with
`--rbf`, no forced timing, and no move repair. Checkpoint settings cannot enable
it. The original `--critical-route-budget` pass keeps its initial position and
behavior.

The final pass runs once after all requested LUT repairs and before final
bitstream/signoff/checkpoint serialization. It rebuilds native physical mux and
analogue cache state, then constructs fresh skewed and reference-free timing
analyses. It captures current live attributes without retaining serializer
side effects. It does not reuse the initial pass's shortlist, candidates or
learned search observations.

Ordinary net/driver/user admission remains as narrow as the initial route pass.
The final pass additionally requires the existing bounded structural/native
source coverage and strict downstream capture coverage in both timing frames.
Unknown, public, asynchronous, cut or incomplete branches reject. The optional
unobserved-arithmetic policy is not enabled at this call site.

The existing candidate generator, two bounded feedback generations, native
worst-sink ranking, route transaction and full-inventory acceptance guard are
shared unchanged. At most 32 full native trials run. Every final trial rechecks
current coverage before routing and after configuration; complete native clock
metrics must remain nonregressing in both frames. Existing setup and related
hold preservation, positive-margin preservation, exact metadata/endpoint
inventory and at least 20 ps improvement of an already failing setup endpoint
remain mandatory. Rejection and exceptions restore the original native tree.

Final-stage logs distinguish shortlist selection, structural/coverage/cache
exclusion, candidate counts, native ranking and final decisions. Learned PIP
costs stay a search heuristic; the table present at final-pass entry is restored
before native timing evaluation and final signoff.

This is a diagnostic, not a coverage waiver or hardware acceptance. Unsupported
rows remain unsupported. The saved-path screen motivates an experiment; it
does not establish native eligibility, available route alternatives or timing
gain. A fresh paired route and complete artifact/endpoint/clock/topology proof
remain necessary.

## Optional native buffer users

`--critical-final-route-buf-users` is a separate default-false option requiring
a positive explicit final-route budget. It changes only the final pass's user
boundary: ordinary FF/ALUT Q roots and both complete timing coverage helpers
remain unchanged. It does not enable the unused-arithmetic policy. Saved
settings cannot enable it; absent flag performs no helper scan or snapshot.

An admitted BUF must be the actual fixed transparent native A/Q shape, with
one E/F half-private input, COMBOUT output and a private routed Q connected only
to the same ALM half's FF.DATAIN. No name suffix is trusted. Native pins,
control caches, local FF data selection, reciprocal users and all cell-port
references are checked without modification. Dynamic SLOAD, SDATA, protected,
public, global, clock, constant, locked or malformed boundaries reject.
Reverse-port scans stop after one million entries; trees stop at 4096 wires.

Before any proposal measurement, read-only snapshots cover every affected
helper LAB occupant, native pin/cache/LAB field, complete configured masks and
selectors, and the exact private Q trees/strengths. After restored measurement
and before/after each full trial these boundaries must match. Only the original
candidate input net's tree may change. Native source-exit or mate-selector
changes reject. The existing net transaction restores routing, configured
muxes and analogue cache after false or exception; it does not claim to undo
arbitrary logical cell corruption. Snapshot mutation tests are detection-only.

No pin allocation, BEL binding, helper insertion or control-set recalculation
is performed by this option. The original candidate generations, ranking,
transaction and complete setup/hold/clock acceptance remain unchanged. Host
eligibility is not evidence of timing gain; enabled/disabled paired full-route
artifact and native endpoint proofs remain mandatory before retaining a recipe.
