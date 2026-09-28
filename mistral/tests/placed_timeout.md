# Placed timeout diagnostic

This private fixture probe applies the formally qualified three-channel timeout
rewrite after ordinary placement and optional enable replication. It replaces
six root LUTs with 41 LUTs from the generated payload; all surviving cells,
including shared old interiors and replicas, retain their BELs and pin states.
It does not change the production placement objective or synthesis defaults.

`NEXTPNR_MISTRAL_PLACED_TIMEOUT=<prefix>` enables the probe and writes before/after
JSON, effective pin-state sidecars, selected sites, and a payload-hash manifest.
An absent or empty prefix returns without allocating identifiers. Inputs resolve
to retained FF Q pins; the generator folds any explicit source NOT parity into
LUT masks. The qualified fixture has no inverted or constant cut inputs.

New LUTs are placed topologically within Manhattan radius six of each channel's
old two-root midpoint. Legal free BELs are ranked by worst predicted bound-input
plus bound-output delay, then distance and BEL name. Future unplaced endpoints
are omitted. This is a bounded heuristic, not calibrated route-delay prediction.
`NEXTPNR_MISTRAL_PLACED_TIMEOUT_REGION=roots` expands only the candidate domain
by adding radius-six neighborhoods around each old root. The original midpoint
region remains included, and the score still uses the original midpoint
distance as its tie-break. An absent or empty region retains the original mode;
when the diagnostic prefix is disabled, even an invalid region is ignored.
Sites and the manifest record the region mode and old-root distances/coordinates.

No original cell is displaced. Failure aborts before routing; the hook does not
provide transactional recovery for reuse of the same context.

`PlacedTimeoutTest.DisabledIsIdentifierNeutral` checks the disabled path.
`PlacedTimeoutTest.ActualSnapshotPreflight` is optional: set
`MISTRAL_TIMEOUT_TEST_SNAPSHOT` to the qualified baseline replication-after JSON
with its `.pins.tsv`, and `MISTRAL_TIMEOUT_TEST_PREFIX` to the evidence prefix.
The test restores effective pin states and one known packed PLL output lost by
JSON scalar/index collision. This is diagnostic import, not supported route
replay; the fresh in-process run remains authoritative.

The generated include corresponds to payload SHA
`74b9712ebcf19a5a51bd1f48210732054a2d29d7d5c406622e9d195893985713`.
Its source candidate SHA is
`d2122f80bce6ab472d8b3663841047939f79670348c867e045f9db59bd080117`.
The reproduction script and independent Boolean/snapshot checks are retained
with FES issue264 diagnostic evidence under `out/ramtest-placed-timeout`.
