# Spectrum qualification evidence

`qualification.json` consolidates the recorded build and run evidence.
Every imported build metadata file, policy log, run log and raw runner
`summary.json` is preserved byte for byte. `SHA256SUMS` covers all stored
evidence files except itself; run `sha256sum --check SHA256SUMS` here.

`test_passed` records the runner's selected assertion. An expected packing
rejection or initial plateau can pass that assertion while
`actual_route_complete` remains false. Final analogue clock gates and
repeat results are reported separately. A timeout that previously reached
zero overuse remains incomplete.

The original-cache raw summary uses `checksum`; later raw summaries use
`last_logged_route_checksum`. Only the consolidated file normalizes the
field name. These are logged Router1 checksums, which may precede later
analogue candidate changes or restoration of an earlier route. Final RBF
hashes and final reported clock values provide separate repeat evidence.

The iteration comparisons select full `iter=... wires=... overused=...
overuse=...` log lines and compare their strings exactly. They reference
the fixture's retained original log and copied replay logs, each with its
SHA-256. A fixed replay can match the first 104 original iterations and
then continue, so a matching prefix and a matching complete sequence are
separate results.

Source archives, compiler binaries, chip database archives, complete
configuration/build logs and routed artifacts stay outside the repository.
Their locations and hashes are recorded in the consolidated evidence and
full build metadata. Neither source fixtures nor device kernels were
modified to prepare this evidence.
