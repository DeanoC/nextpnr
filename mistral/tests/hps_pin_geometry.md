# Analytical HeAP HPS pin geometry diagnostic

This branch adds an opt-in placement experiment based on71c513. It does not
change routing delays, clock timing, SA refinement or normal compiler defaults.

Set `NEXTPNR_MISTRAL_HPS_PIN_GEOMETRY=/existing/directory/prefix` before a full
HeAP placement. An absent or empty variable leaves the callback unset and does
not allocate identifiers. An enabled experiment without applicable connected
fabric pins fails explicitly.

Mistral caches relative offsets for FPGA2SDRAM logical ports before HeAP runs.
It obtains each physical pin's GIN/GOUT coordinates from `getBelPinWire` on the
unique architectural HPS BEL, independently of the cell's current binding.
The analytical variable retains its ordinary BEL coordinate; its endpoint
coordinate is variable+offset. This matters because HPS participates in HeAP's
solve buckets even though strict legalization has only one physical BEL.

The offsets affect analytical net extrema, distance weights, equation RHS
terms and HeAP's HPWL/convergence metric. Both solved and fixed variables
contribute `-weight*offset` to the RHS; fixed variables also contribute their
existing `-weight*cell_position` term. Zero offsets add no extra RHS operation.
Endpoint coordinates are not clamped. Legalization, spreading and SA retain
their existing cell coordinates, and `predictDelay` remains unchanged. This
is explicitly an analytical-phase experiment, not a complete correction of
every placement cost function.

The pin manifest records every considered HPS port, its logical and physical
pin, wire, BEL, state, offset, application status and exclusion reason. Clock
inputs, folded constants, MISTRAL_CONST-driven/constant nets, unused outputs,
unconnected pins, missing/ambiguous mappings and non-GIN/GOUT wires are
excluded. Unused outputs have no HeAP equation and are recorded as such.
PIN_INV signal pins retain their physical coordinates and polarity.

Evidence files are `<prefix>.pins.tsv` and `<prefix>.scope.json`. The former
allows comparison against the independent device port database; the latter
records the analytical-only scope and application counts.

Focused tests:

```sh
build/nextpnr-mistral-test --gtest_filter='HeAPPinOffset.*:HpsPinGeometryTest.*'
```

These exercise the production equation helper with fixed, solved and jointly
solved endpoints, zero-operation preservation, negative/unclamped coordinates,
actual ready/input/data mappings, binding-independent caching, exclusions,
no-op rejection and disabled identifier neutrality. Real no-route placement
must also match the complete baseline module with the diagnostic off. Full
fresh placement/routing remains necessary to measure the enabled result.
