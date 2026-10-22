# PC/XT leased replication comparisons after budget reset

The user reset the incident budget on 22 October and authorized continuation.
The renewed allowance is 21,600 seconds. Previous failed and unleased captures
remain preserved and charged to their original incident ledger.

This is a diagnostic comparison, not timing closure or hardware acceptance.
FES base is `08edf4fc`; driver/evidence base is nextpnr `bbd7ce1b`. The native
binary remains the frozen `c8d4a1d3` build, SHA256
`323269dc55ec9610298ff297ba6439f78e70a9596f168c168f405e0d5d34ce29`.
No compiler source, RTL, clock, recipe, lock, seal or shared contract changed.

## Complete leased routes

All live GPU routes use the repaired FES phase-lease path with grant records.
The baseline and three-copy candidate use identical frozen inputs, binary,
seed 1, weight 2000, exponent 5, QSF/SDC and 100 MHz target. The 64-trial candidate
uses those same inputs and the already completed baseline; its baseline copy
is hash-recorded and is not rerouted or charged twice. Each invocation has a
600-second active limit; queue waits pause that limit. The ledger conservatively
charges full wall time, including waits and overlapping CPU diagnostics.

| Final setup WNS (ns) | Baseline | Three copies / 32 trials | Four copies / 64 trials |
| --- | ---: | ---: | ---: |
| CPU/system | -1.638 | -1.516 | -1.532 |
| Microcode phase | -0.358 | -0.258 | -0.080 |
| Memory | +2.854 | +2.854 | +2.854 |
| Pixel | -0.228 | +0.269 | -0.035 |
| Failing setup rows | 343 | 252 | 337 |
| New setup failures against baseline | — | 18 | 59 |
| Resolved setup failures against baseline | — | 109 | 65 |

All three inventories have 17,199 matching endpoint keys, unchanged clocks and
IO constraints, zero failing related-hold rows and 105 unsupported boundaries.
Coverage remains incomplete. Both candidates are rejected; the driver retains
the baseline bundle. Independent comparisons also show setup/hold margin
regressions, including positive margins, so resolving coverage alone would not
make either candidate acceptable.

The three-copy candidate's worst CPU path is still last-result to microcode
read-address, with 8.413 ns in routing and 2.555 ns in logic. New failures include
a framebuffer RAM path (+2.264 to -0.749 ns) and CPU/BIU paths. A local netlist
improvement followed by full rerouting changes unrelated paths substantially.
The four-copy candidate adds one exact ALUT5 copy, improves no worst predicted
clock margin and creates more new routed failures. These bounded results do not
justify further increases to replication trial count on this placement.

The next compiler investigation should target local route preservation and
routed evaluation of the affected data cone, with rollback and the existing
full-design endpoint/hold/clock guards. This is a proposed direction, not an
implemented or qualified repair. No seed sweep or production promotion follows
from these results.

## Cross-checks and evidence

The leased baseline's RBF, routed JSON, timing and endpoint JSON are byte-identical
to the prior unleased baseline. That prior run remains unleased; this is a new,
properly leased reproduction of its result.

A CPU-reference baseline timed out at 600 seconds and has no final artifacts.
The CPU-reference three-copy candidate completed with timing failure. Its RBF,
timing JSON and endpoint JSON are byte-identical to the live GPU candidate.
Routed JSON differs only by a bijective renumbering of 15,738 bit identities and
the CPU-backend setting; the independent normalization checker verifies all
remaining content exactly. CPU-reference evidence remains diagnostic.

The 64-trial host search tests 247 geometries, accepts four exact copies and
passes the independent whole-netlist substitution checker. The earlier 55
native, 17 CLI and 28 retention/runner test results still apply to unchanged
implementation; they were not rerun for these evidence-only changes.

`leased-capture.tar.gz` contains raw logs, reports, RBFs, commands, receipts,
inputs, host equivalence results, comparison scripts and the full renewed ledger.
`leased-manifest.json` hashes every archive member; all members were verified by
readback. Binaries are excluded, with source identity and hashes retained.
The four-copy baseline is recoverable from the three-copy comparison baseline.
No kit was claimed or programmed. Timing issue FES #840 and draft PRs nextpnr
#266/FES #979 remain open.

The renewed ledger charges **2703.542323 seconds** and leaves
**18896.457677 seconds** (about 5 h 15 min). Archive SHA256:
`3c4c87f5542d406f6c7570515b29f009ffce54f246640022f38f99331903f403`.
