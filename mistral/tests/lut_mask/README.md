# Emitted LUT masks for FF DATAIN routes

Run `check.py --yosys <yosys> --nextpnr <nextpnr-mistral>
--mistral-cv <mistral-cv> --output <directory>`.

This small fixture routes a seeded shift/XOR sequence plus initialized writable
MLAB storage. The check decompiles the actual RBF and verifies FF DATAIN
`MISTRAL_BUF` truth tables in both ALM halves, NOT inversion, an ordinary XOR LUT,
and every initialized memory bit. It checks physical mask bits rather than
assuming routed JSON proves correct configuration emission. The fixture's
half-specific F input occupies truth-table address bit 4, E0/E1 bit 3 and C/D
bit 2. The XOR's expected mask follows the physical pins recorded for it, which
are F with E, or C/D when a neighbouring flip-flop keeps E. CRAM storage is
active-low. MLAB storage uses its known physical permutation. A route-through
buffer on F is `0xFFFF0000`.

The frozen-scaffold compiler guard introduced in 6916e9d2 excluded MISTRAL_BUF
from mask evaluation, turning pass-through LUTs into constant masks. Before the
fix this regression fails with `0xffffffff` instead of `0xf0f0f0f0`, even though
routing succeeds. Buffer classification for placement is intentionally unchanged.

The defect was independently isolated using the same full Catch diagnostic
synthesis/QSF/SDC with nextpnr 0fad53a7 and d672fade: routing/timing matched, but
74 emitted LUT masks differed. Contained hardware sampling found both diagnostic
clock sequences changing with 0fad53a7 and fixed at their initial seeds with
d672fade. That diagnostic is supporting evidence, not a hardware test performed
by this host-only regression.
