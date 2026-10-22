# Route-through FF cache correction

Route-through insertion rewires an FF's DATAIN port through a local BUF. It
previously left `ffInfo.datain` pointing at the old fabric net, so native
legality and input counting treated the same input as an independent FF input
and a LUT input. Commit `972d8ea6` refreshes the changed FF cache before binding
the helper recounts the ALM. The legality rules remain unchanged.

The full-board native replay starts with no mismatches or illegal LABs. Old
route preparation creates 1,949 stale FF caches, 309 illegal LABs, 411 illegal
ALMs and 188 overfull cached LAB counts. The corrected preparation leaves zero
in all four categories. Its complete observed snapshot equals the old probe's
diagnostic refresh, and the three previously blocked CPU LABs retain 42 inputs.
No routing or STA is performed by this structural probe.

Eight focused native regressions fail on old objects and pass with the fix.
The frozen combined build also passes 33 LUT6/coverage cases, 77 private-pair
cases and both six-case CLI suites. These are suite counts, with overlapping
coverage cases. Default and explicit-zero full-design placements are byte
identical; their complete modules and predicted timing match `461ce654`, with
only the creator label changing. The private-pair implementation in this build
is commit `7b8556ad`; it is not enabled in these controls.

The 85-member archive includes source identities, frozen source files, old and
fixed native probes, expected failures, validation logs, complete placement
controls and review records. Its SHA-256 is
`121dd4407e48945bd49ed19a9bfdfee791d05d6483ae8548f5ec5b119d2e6caa`.
The executable is identified by SHA-256
`ab3a8e6e50c90be23ea5129b3f6fb8d7092310002a96c7cf0a07c13a83f71ef4`.
Executables, linked objects and test libraries are omitted; the captured
commands and hashes identify them.

After extraction, `python3 verify.py` verifies all retained file hashes, the
19 frozen source identities, test-log counts, complete native snapshots and
full placement controls. Fresh archive readback reproduces its result exactly.
This rechecks retained evidence without rebuilding or reexecuting native tests.

No full route ran with this build. Enabled LUT6 routing is explicitly blocked
by a separate upstream-origin admission gap: a functional missing-clock or
undriven source can disappear from propagated timing tags while another source
leaves complete-looking downstream rows. The new source guard must reject
those omitted origins before the next enabled pair.

This is host-only structural/model evidence, with zero added route charge.
There is no new routed timing result, seed qualification, production seal or
hardware acceptance.
