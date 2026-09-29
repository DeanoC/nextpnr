# Open ACLR sharing a live LAB clear

One flip-flop has an async clear. The other has clock and data only.
Both read the same data pin so placement keeps them in one LAB.
The open flop must select the unused ACLR slot, and that slot must
drive the dedicated inactive clear rather than the other flop's DATAIN.

A BEL lock is not used. The open flop and the cleared flop share one
LAB without a site lock, which is the placement this test checks.

The second check rewinds the saved `FES_LABSTATE_V1` index to the
pre-fix slot and reloads it with `--fes-scaffold --no-route`. That
reload does not run `lab_pre_route`, so the restore itself parks the flop.
A snapshot that marks both ACLR slots used is rejected.
