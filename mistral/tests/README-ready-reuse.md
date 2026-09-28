# Existing ready-enable replica reuse diagnostic

`NEXTPNR_MISTRAL_READY_REUSE=<prefix>` enables one fixture-scoped pre-route
connection change after the existing placement diagnostics. It moves the sole
original-enable ENA user in LAB34,22 (`skid_burstcount[5]`) to the already placed
ALUT3 replica in LAB37,25. Both drivers must have mask0x32, identical A/B/C nets
and SIG pin states. All111 users must be ordinary same-clock FF ENA inputs.
The source fanouts must change107/4 to106/5. No LUT, net, input load, placement,
FF parameter or other connection changes.

The hook refreshes the moved FF's control metadata and its ALM input count,
checks every occupied BEL, and proves all unaffected indexed users and pin maps
unchanged. Fresh memory-clock constraints are mandatory. Before/after absolute
224-endpoint timing is diagnostic; predicted setup is not an acceptance gate.
The hold guard disallows a new or worsened predicted negative margin.

After normal bitstream configuration, a separate read-only hook exports the
same224 endpoint setup/hold/domain rows and every clock using the baseline
TimingAnalyser configuration. It asserts that the complete graph, physical
routing, pin states, cache and calibration remain unchanged. Measured regressions
are recorded for external comparison, never hidden or used to reject evidence.
There is no route candidate generation or rerouting in this hook.

The tests exercise actual LUT input/mask/polarity guards, missing/mismatched
clock constraints, the local FF metadata update with another ENA in the same LAB,
and disabled identifier neutrality. The optional existing snapshot test restores
carry metadata, known PLL import omissions and an explicitly declared130MHz
memory clock, then applies timeout mapping, retained replication and this change.
That preflight is a predicted diagnostic, not an exact context import or route
replay; a fresh full build and independent timing comparison remain authoritative.
