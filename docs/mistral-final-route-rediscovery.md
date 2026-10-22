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
