# PC/XT analytical placement trace

Compiler `36b9e372` adds diagnostic output behind the existing `--debug-placer`
switch. It records solved and spread targets, legalizer dispatch priority and
chosen BELs. No placement policy, timing model or acceptance check changes.

Four CPU-only fresh pack/place runs compare parent compiler `cbb1e1f1` and the
instrumented build, each with debug disabled and enabled. Every run exits zero
and produces the same complete placed JSON bytes, SHA256
`9a7e918fa10816815cb986e536a512496cd69906ad577075ee64576450957930`.
The capture retains one shared output, four independent receipts and full logs,
the exact source diff and binary hash. Initial attempts through `gpu-run
--phase-lease` completed nextpnr but the wrapper returned 70 because placement
never requested a GPU phase; those receipts and log tails remain recorded.
The CPU-only replays establish successful execution without a GPU lease.

The minimum-wirelength HeAP solution is iteration 6, legal HPWL 248409.
Its sixteen microcode ROMs have mean solved target (14.64, 25.97), then mean
spread target (41.26, 65.25). Every ROM is dispatched at priority 1 and bound
in column 41. Subsequent simulated annealing moves them to columns 38/41 near
rows 34–43. Thus the large displacement already occurs during spreading;
changing the strict legalizer queue alone does not address that stage.

Source review also verifies that related-clock phase and M10K setup time are
modelled: the CPU-to-ROM address window is 2.5 ns and M10K address setup is
0.125 ns. The timing analyzer normalizes placement criticality within a clock
domain pair, so phase alone does not increase its urgency. That observation
motivates further experiments; it does not justify changing timing constraints.

The spreader review identifies two independent indexing defects: its one/two
cell interpolation uses the absolute vector index instead of the index within
the selected half, and rightward pivot movement updates the previous cell's
bucket rather than the moved cell's bucket. The trace does not prove either
explains all PC/XT displacement. A separately tested, default-off correction
is the next experiment before introducing new placement weights.

`capture.tar.gz` has 27 verified members, SHA256
`022ee12955c5955068c00de9322fefab6b6e0a411dc44317eb543d4b7a08789e`.
All members pass size/hash readback. Original synthesis and constraints are
retained with the sibling balanced-reduction capture. Captured absolute paths
must be adjusted to extracted inputs and binaries rebuilt from recorded source.
No routes were run for this trace, no route time was charged, and no hardware
ran. This is host-only diagnostic evidence. Production clocks, RTL, recipes,
locks, seals and shared contracts remain unchanged; timing and physical gates
remain open.
