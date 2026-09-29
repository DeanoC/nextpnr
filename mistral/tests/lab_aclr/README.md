# Open ACLR sharing a live LAB clear

One flip-flop has an async clear. The other has clock and data only.
Both read the same data pin so placement keeps them in one LAB.
The open flop must select the unused ACLR slot, and that slot must
drive the dedicated inactive clear rather than the other flop's DATAIN.

A BEL lock is not used. User BEL strength skips LAB control-set
assignment, so the mux this test checks would never be written.
