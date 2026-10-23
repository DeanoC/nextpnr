# Separate final ordinary-LUT move stage

Proposed scope: a default-zero final-stage caller of the existing ordinary LUT move transaction, after the existing LUT repairs and final-route pass. This opt-in is diagnostic only. Host proof does not establish routed gain or hardware qualification.

The saved best result still has eight setup failures, including two CPU captures at −25 ps. The completed BUF-enabled final reroute admitted ALUout[3], but its three routes were slower and no trial was retained. A native structural preflight found legal empty destinations for the already-eligible penultimate CPU ALUT4 and three CGA LUTs, with exact false/exception rollback. It establishes geometric possibilities, not native weighted cost, coverage, a live shortlist or gain. The PIC legal placements do not override the actual cut/untimed source exclusions.

## Interface and default behavior

- Add `--critical-final-move-budget 0..16`, absent/zero disabled, stored only in an Arch runtime field. Serialized settings cannot enable it.
- Add a distinct `Arch::repair_final_critical_moves()` hook after `repair_critical_routes(true)` and before `build_bitstream()`. Its first operation is an immediate return on zero, before IDs, snapshots, configuration or analysis.
- Positive admission requires a fresh ordinary full GPU route with RBF output, normal unforced timing, no reload/no-pack/no-place/no-route/pack-only/cart/scaffold/reserved/frozen/CRAM paths, and a positive preceding LUT-repair or final-route budget. Reject simultaneous legacy `critical_move_route_budget` to keep execution unambiguous. Other prior repair budgets may coexist.
- Include the new budget in the final checkpoint-refresh predicate. Keep the old move option, discovery, router and transaction behavior unchanged, including their present CLI conflicts.

## Candidate footprint and bounded search

Reuse the existing `ordinary_lut`, `protected_cell`, `fixed_pin_target` and `footprint` functions byte-for-byte. Movable cells remain ordinary ALUT2..6, unclustered/unprotected/weakly bound, with existing fixed pin maps and empty compatible same-half destinations. The incident footprint remains at most seven routed data nets, at most64 users per net, existing strength/public/clock/protection restrictions, and only ordinary LUT-input or FF.DATAIN consumers. Do not admit BUF users, ARITH consumers, carry-chain moves or M10K/BUF roots through a new exception.

Use the current live native slack, at most32 negative-slack cells, the existing cell-name tie break, current first-five anchors (including home), 3×3 neighborhoods, original weighted Manhattan score, target-ID tie break and at most four legal destinations per cell. Keep the existing round-robin destination ordering and total16 trial ceiling. The geometric preflight's all-neighbor superset is evidence only; it is not substituted for this selector. Keep the shared500000-node local-router bound and no-foreign-ripup rule.

The selector runs only after all preceding repairs, using newly configured full timing. Before every actual trial, recheck current footprint, destination emptiness/legality and coverage against current retained state. An earlier acceptance may make a later proposal stale; stale proposals must reject before incrementing the trial counter. A retained cell's list is cleared as in the original selector. The score is proposal guidance only.

## Modern source and capture coverage

Construct full setup/hold analyses in both skew-aware and reference-free frames, with no timing loops. For the moved ordinary LUT, call the existing strict `critical_lut6_swap_source_coverage` for ALUT6 or `critical_lut_permutation_source_coverage` for ALUT2..5. These traverse every actual fanin and compare complete native launch relationships; the PIC cut/untimed/duplicate relationships remain rejected.

For every incident net, call the existing strict `critical_lut6_swap_output_coverage` in both frames. This checks all downstream branches and characterized FF/M10K captures. It deliberately ignores the global unobserved-arithmetic opt-in: unused SO/CO, public/unknown/async/cut branches still reject. A source ALUT_ARITH is reached through the target's existing upstream walker; it is not admitted by widening `critical_final_route_source_coverage`'s ordinary-Q root scope. This follows the established permutation pass's target-source plus all-input/output capture boundary.

Run these checks before shortlist eligibility, again immediately before mutation, and after actual routed configuration. Reuse `private_pair_clock_safe` in both frames, the live complete endpoint inventory, and `critical_route_snapshot_safe`: exact clock/constraint/coverage identities, no setup or hold regression (including positive margins), and the existing minimum20 ps improvement requirement. Never compute a timing acceptance result against mutated pins and old routes.

## Transaction and physical boundary

Keep `critical_routed_move_trial` unchanged. It already saves all incident trees, retains every original input source-to-stationary-user path and strength, moves only one LUT through the native placement transaction, and reroutes the moved output from its new source. The new final caller adds a separate snapshot/inspection wrapper; it does not silently strengthen or broaden legacy callers.

Acquire the wrapper snapshot before any placement preview, including legality probes during ranking. Capture current cell identities/types/BELs/strengths/ports/params/attrs/pin maps/ArchCellInfo, exact driver and indexed-user stores including holes, net aliases, all LAB state, canonical wires/PIPs/strengths, configured RBF and keyed analogue cache/validity. This is an enabled-only cost. Placement previews must restore the source and leave the previously empty target unbound; false and exception paths must match the complete snapshot before discovery continues.

For an actual retained candidate:

- Only the selected cell's native BEL changes. Type, parameters, logic, logical connections, indexed slots, physical pin names/states, protection and every other cell binding remain exact. No helper/controlset/pin-allocation functions are called.
- Source/destination native occupancy and counts may reflect that one move. Validate their exact relation to the original snapshot and native recomputation; do not waive unrelated LAB fields. All stationary FF controls/input selectors and combinational mate maps/masks remain exact. Whole-LAB legality is mandatory.
- Compare the complete configured LAB/MLAB bmux field inventory conservatively, including stationary source LABs outside the two moved ALMs. The two selected ALM LUT_MASK records may differ, with the following explicit native-mask proof: enumerate every assignment of the actual distinct raw input nets (at most64 rows), respecting PIN_INV, constants and correlated aliases, and require the old source-half output to equal the new destination-half output. Independently compare every bit belonging to an occupied stationary mate half in both affected ALMs; excluding an entire LUT_MASK record from the static comparison does not exempt its stationary half. Incident ordinary-Q direct COMBOUT output selectors may change only under the ownership proof below. A completely empty affected ALM may transition between the exact native empty LUT_MASK (MT_RAM/default/64/eight zero bytes), TMODE D_E and BMODE C_E defaults and the ordinary active E_0/E_1 modes with all unused-half mask bits one. This requires every LUT/FF BEL empty and every COMBOUT, FFIN, FFOUT, carry/share ingress/egress identity present and unowned in both route inventories for the empty frame. The active frame must contain only the selected ALUT2..5, with no FF or mate. All complete field tuples and moved truth still require validation. There is no arbitrary mask/mode or whole-ALM exemption. An FF-only ALM remains active and receives no empty exception. Every other configured selector/control/mode field stays exact. ALUT6 and other native mode transitions remain outside this scope. The observed CPU/CGA candidates are ALUT4/5.
- Before any mutation, prove that vacating the source half cannot remove a stationary local COMBOUT consumer. Inspect all native FF occupants of the source ALM, exact DATAIN/SDATA nets and physical input identities; reject a same-half FF using the moved Q through its local COMBOUT path. A distant route cannot replace that direct input under this selector-preserving scope. After routing, inspect both affected ALMs again and reject any stationary FF changing between local and external input selection. The destination half must not acquire an unproved local consumer.
- Preserve all stationary input trees/strengths and nonincident trees exactly. The moved output can change its entire tree, but each actual source/sink identity and complete route must remain valid. Inspect every changed native TDFF/BDFF selector and every unchanged output owned in either frame by an incident logical-Q driver on the corresponding COMBOUT BEL. Require a unique incident owner, saved/live driver identity and Q PIN_SIG mapping to exactly COMBOUT, one physical source root and the actual direct COMBOUT-to-output PIP. Compare complete typed/default/data tuples: NLUT for an owned output and native REG default for an unused output. FF-origin output changes and foreign ownership reject. Equality of selector values never bypasses ownership validation. Index LAB identities and selected-output route owners once, retaining duplicate-owner rejection; touched-output discovery examines only the bounded incident footprint. PKREG, clocks and input selectors remain exact. This is not the separate source-exit retention policy.
- Before any placement preview, validate original incident-Q output selectors and complete empty-target canonical fields/observer exclusion. Malformed original RBF fields reject without mutation, preventing a preview rollback from normalizing them.
- Check static state before STA, then full coverage/clock/inventory guards. Rejected and throwing trials must restore original bindings, all LAB fields, every incident route, configuration and native cache exactly before another trial. The old transaction performs its restore; the outer snapshot verifies it. Any unexpected restoration mismatch is a hard error before bitstream output, not a reason to continue from a corrupted state.

The initial native run exposed a boundary error: routing the moved Q changes its own TDFF/BDFF output selectors. The ownership rule above addresses that concrete gap and received a separate narrow source review. Native tests decode actual RBF truth and selectors across LAB/MLAB and both halves, exercise false/exception rollback, and inject selector/mask/cache faults. Host results remain separate from full-route admission and timing qualification.

## Proposed files and test API

Production changes should stay in `mistral/routed_move.cc`, `arch.h`, `main.cc`, and `analogue_repair.h`, with one new focused backend test, CLI test additions, CMake registration and a dedicated diagnostic document. Keep the legacy source sections unchanged except the one checkpoint-refresh budget term.

A narrow native test entry point can wrap the existing trial:

```cpp
bool critical_final_routed_move_trial(Context *ctx, CellInfo *cell, BelId target,
    int visit_budget, const std::function<bool()> &accept, std::string *reason);
```

The production caller always passes500000. The wrapper owns exact state/config inspection and rollback validation; orchestration owns fresh timing and acceptance. No public new general placement framework or CLI node budget is proposed.

Focused fixtures must cover LAB/MLAB and both halves, occupied stationary mates and local FFs fed by the stationary mate (not the moved Q), exact emitted LUT truth/PIN_INV, retained shared stationary branches and indexed holes, first/second incident routing failure, false/exception callbacks, deliberate configured-selector/cache faults, and original/target BEL restoration. Include a required rejection/rollback fixture for a source same-half FF fed by the moved Q, plus destination local/external selector transitions, stationary-half mask corruption, and each old/new moved-function truth-row corruption. No PKREG exception is permitted to make such a case pass. Include coverage negatives matching the known PIC omitted/cut origin case, hidden async/public/dead-carry branches, positive hold regression, missing/extra endpoint rows and both clock frames. Logical-corruption tests are detection-only unless the wrapper explicitly restores that field; do not claim the original route transaction undoes arbitrary external edits.

Retain existing move/router fixtures and default/explicit-zero CPU-only byte controls against the frozen binary. Full-route admission requires reviewed frozen source, binary and host proof; the later matched default/final-move pair alone determines whether this helps. No current result is timing or hardware qualification.

The subsequent full-board move16 diagnostic kept no moves: four actual CGA trials hit the stationary-mate-mask check while vacating a sole-LUT ALM. Logging-only native replay established the canonical empty defaults and direct incident input-source COMBOUT exit changes. The bounded exceptions above address these observed native writer transitions. They inherit no timing gain or hardware acceptance; source review, native regression and a new matched whole-route comparison remain separate gates.

## Optional bounded rediscovery

`--critical-final-move-rediscover` is a runtime-only, default-false flag requiring
positive `--critical-final-move-budget` and its unchanged fresh full GPU route
admission. After the first pass finishes, a second discovery runs only if a move
was retained and the original trial budget has capacity. There are at most two
discoveries and at most the original 16 trials total; budget is never reset.

The second discovery rebuilds both timing frames and the endpoint inventory from
the retained placement, then repeats the same eligibility, geometry, legality,
caps, ordering and transaction. Every coverage and timing guard remains in force,
including positive setup/hold margins. Enabled runs log discovery pass, cell and
candidate counts, remaining budget and discovery time. Disabled runs perform no
extra analysis or discovery and emit no additional log records.
