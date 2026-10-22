# PC/XT shared-data LUT replication diagnostic

Compiler source `c8d4a1d36907780ef717fae7312bf44d834f8d75` adds explicit,
default-off replication of a shared ordinary data LUT for one consumer. The
frozen binary SHA256 is
`323269dc55ec9610298ff297ba6439f78e70a9596f168c168f405e0d5d34ce29`.
FES base is `cf37285b`; the compiler base is `d133140a`.

The host search tries 32 native STA candidates and retains three equivalent
replicas: one ALUT3 and two ALUT4s. Every accepted copy passes the existing
full-design endpoint, clock, related-hold and setup-gain checks. The worst
predicted clock slack remains unchanged. The compiler's disabled result is
byte-identical to the earlier disabled placement.

`proof.py` independently verifies the emitted netlist against the baseline:
copy type, truth table, input nets and pin polarities match; replacing copy
outputs with their original equivalents restores every field of all 10,448
original cells, every original net and all module-level metadata. Original
registers and their initial state are unchanged. A changed-mask mutant is
rejected. This is whole-netlist equivalence by exact combinational substitution,
not a new CPU pipeline or a hardware acceptance claim.

Validation passes all 55 native CriticalCohort tests, all 17 CLI tests and all
25 routed-retention tests. The initial positive test exposed exhausted guidance
after its sole edge was replaced. The fix permits clean exhaustion only after
an accepted copy; malformed initial guidance still fails. Truth-table and
rollback/exception tests passed before that fix and pass in the full final run.

The route experiment uses one immutable placed checkpoint, compiler, seed 1,
weight 2000, exponent 5, device, QSF/SDC and clock constraints in two isolated
processes. Only the candidate enables `--critical-swap-replicate` with budget 32.
Each route had a 270-second cap; the starting incident allowance was
607.674011 seconds. Both invocations were accidentally unleased: the driver
copied the executable as `compiler`, while FES recognises only the basename
`nextpnr-mistral` for GPU wrapping. Calling `_run_command` alone did not lease
these jobs. These captures are **not managed-GPU qualification evidence**.
This checkpoint-to-route experiment cannot be compared as a matched pair with
earlier synthesis-to-route captures.

The baseline completed with timing failure. CPU/system setup WNS is -1.638 ns,
microcode -0.358 ns and pixel -0.228 ns; memory is +2.854 ns. Its inventory has
343 failing setup rows, zero failing related-hold rows and 105 unsupported
boundaries among 17,199 endpoint keys. Coverage remains incomplete. The worst
CPU address path spends 8.014 ns in routing and 3.045 ns in logic.

The candidate retained the three copies during placement but timed out during
analogue repair. It has no final timing, endpoint inventory, routed graph or
RBF. Interim timing is not a comparison result. The driver retained the failed
baseline bundle; this is fallback artifact retention, not acceptance.

Both runs remain charged: 197.087091 seconds baseline plus 270.120800 seconds
candidate, 467.207891 seconds total. **140.466120 seconds remain** in the
incident allowance. No rerun was attempted with that insufficient paired-route
budget. No hardware ran, and no toolchain lock, recipe, clock, seal or shared
contract changed. Timing issue FES #840 remains open.

Runner repair `f6a234dd` preserves the executable basename, checks the FES
runner's recognition before dispatch, and requires grant records before a GPU
receipt may count as completed. All 28 retention/runner tests pass, including
three new host-only dispatch regressions. An additional check against the real
FES recognition/grant helpers passes; no GPU is dispatched by these checks.
The repaired runner has not yet been exercised in a live leased route.

`capture.tar.gz` preserves inputs, commands, receipts, complete baseline route
artifacts, failed candidate logs, host netlists, independent proof and tests.
Compiler binaries are excluded; `host/source-identity.json` records their
source/tree and SHA256. `manifest.json` hashes every archive member; every
member was read back and verified. Raw receipts and logs remain unchanged.
`routes/comparison.json` explicitly annotates the unleased execution and
missing candidate final timing. The experiment used compiler source c8d4a1d3;
the subsequent Python runner fix does not change that binary's identity.

## Subsequent leased comparisons

After the user's budget reset, complete three-copy and four-copy leased
comparisons both failed retention. See [the leased follow-up](leased.md) for
final timing, new failures, CPU-reference cross-checks and the renewed ledger.
