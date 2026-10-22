# PC/XT connected-LUT pair placement diagnostic

The new default-off `--critical-swap-pairs` search completed 32 native STA trials
and retained zero moves on the current PC/XT placement. This is not timing
closure or routed evidence. No route allowance was spent; 607.674011 seconds
remain. No hardware ran.

Compiler base: `e4949e6e`; FES base: `df387eb4`. The frozen final compiler hash is
`870d2ef6a476e079374b5b5b356155ae86155de0cbff91875ee5653cf53ce908`.
The archive records hashes of every changed compiler source/test file, because
the binary was built before the result commit and its version label is not a
source identity. Input placement is the previous no-move CPU-locality result;
guidance selects the same 49 CPU microcode read-address paths. Seed, clocks,
SDC/QSF and the existing physical-pin/endpoint/hold guards are unchanged.

The search examined 2080 geometries within its 4096 bound. Several trials
improved a guided endpoint but regressed another protected endpoint. The new
diagnostic identifies the first such setup row, including:

| Protected endpoint | Before | Rejected trial |
| --- | ---: | ---: |
| CPU microcode PC `eu_rom_address_MISTRAL_FF_Q_1.DATAIN` | +0.264 ns | -2.370 ns |
| CPU microcode PC `eu_rom_address_MISTRAL_FF_Q_10.DATAIN` | -0.185 ns | -2.755 ns |
| ROM byte 0 `read_address_MISTRAL_FF_Q_9.DATAIN` | -1.225 ns | -1.447 ns |

These are placement-model margins, not final routed measurements. The CPU PC
and the ROM address copies compete for shared next-address logic. A useful next
investigation is selective replication of that shared combinational logic for
its consumers, with equivalence and all timing guards retained. This result
neither proves replication will work nor rules out other legal placements.

The original frozen compiler, the new compiler with repair disabled, and the
new paired-search run produce byte-identical placed and timing JSON. Both LUT
moves and displaced occupants are rolled back on rejection. Registers, carry
chains, truth tables and nets remain unchanged. The existing isolated route
driver exposes `--pairs` only on its candidate; full routed retention remains
fail-closed, including incomplete endpoint coverage.

Validation: all 49 pre-existing native CriticalCohort tests passed. The initial
new positive fixture used a non-LAB coordinate and failed; its corrected three
pair tests pass, including atomic rollback/exception handling, protected and
overlapping destinations, and improvement through the production search/native
STA. The final frozen-binary CLI (16 tests) and routed-retention (25 tests)
regressions pass; their logs are included. An earlier CLI invocation raced executable replacement during a build;
only the frozen-binary rerun is counted as validation.

`capture.tar.gz` retains the input placement, selected guidance, QSF/SDC,
commands, logs, receipts, tests and one copy of the identical output placement
and timing report. `manifest.json` inside authenticates each member. Compiler
binaries are not embedded. Absolute paths in saved commands describe the actual
run; restore the archived inputs and update those paths to replay elsewhere.
