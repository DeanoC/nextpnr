# Unregistered SDRAM DQ reference

The native regressions are `IoDelayTest.Unregistered*` and `UnregisteredDq.*`.
`reference_check.py` audits all 16 exact GOUT data/OE and GIN input boundaries,
configuration selectors, complete pad buffers and four-corner transition coverage.
`reference_negative.py` corrupts reports/configuration and requires rejection.

The valid reference projects and native receipts are retained in
`docs/validation/2026-10-20-unregistered-pads-265`. Extract its reference archive
and run these scripts in the extracted directory. Its summary documents the
separate BYPASS configuration requirement, exact native selector delta,
reference-only scope, HPS launch gap, and unsupported high-Z turnaround timing.

`prepare.py` creates the two initial 0/30pF small fixtures. Compile them with
Quartus17.0.2 using a600s timeout per fit and query `paths.tcl` after compilation
finishes. Back-annotate a copy of the0pF fit with
`quartus_cdb top --back_annotate=routing` in `routing-probe`; `prepare-targeted.py`
then creates `dq-targeted0` with explicit native VIO boundaries and wildcarded
upstream/downstream routing. The loaded copy changes only DQ far capacitance
to3e-11. RCF entries for duplicate registers optimized away by the second fit
are not claimed as preserved; exact native path reports establish coverage.

No reference constraint is a measured SDRAM/board budget. Seven new fits were
attempted: two rejected malformed-QSF fits, four valid DQ references and one HPS
I2C reference. Do not rerun a full PCXT route as part of this fixture audit.
