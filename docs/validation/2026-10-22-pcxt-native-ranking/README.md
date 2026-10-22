# PC/XT native-ranked route generation

This continues the guarded post-route experiment from nextpnr `3e518286`,
using FES base `588fc58e`. The CPU/system and microcode targets remain 100 MHz,
with pixel 74.25 MHz. This is CPU-reference host diagnostic evidence only.
No hardware ran and no production lock, RTL, recipe, seal or contract changed.

## Native ranking and whole-tree reservation

Compiler `0c02e81a67950fa16176acdcf6842ae81e67414e` reserves two candidate slots
for whole-tree alternatives on multi-sink nets. Ordinary candidate-router
callers retain their old policy. Every generated proposal is evaluated with
native analogue timing for all sinks, including branch loading and terminal
LUT input delay. A conservative worst-sink prefilter requires at least 20 ps gain
before the existing full-design setup/hold/clock/coverage guards can retain it.
It is not a substitute for those guards. Rejected ranking trials restore their
wire/pip bindings and physical mux/cache state.

The same-binary baseline/candidate comparison completes. The baseline reproduces
prior RBF, timing and endpoint bytes; its routed graph matches after bijective
bit renumbering and backend normalization. The candidate generates 12 proposals,
all worse by 99–2017 ps at the selected nets' worst sinks. No distinct whole-tree
alternative is emitted on this cohort despite reserved slots. No proposal reaches
full-design STA and no change is accepted. Independent comparison verifies all
12,605 cells and 15,736 nets unchanged and all 17,199 endpoint rows identical.
CPU setup WNS remains -1.638 ns, microcode -0.358 ns and pixel -0.228 ns. There
are 343 failing setup rows and 105 unsupported boundaries. Retention rejects
incomplete coverage independently of the absence of timing gain.

Twelve focused native ranking/route/rollback tests and 29 retention/runner tests
pass. The first-generation comparison charges 581.498242 seconds, leaving
15685.620510 seconds in the renewed incident allowance.

`capture.tar.gz` includes frozen inputs, source/binary identities, commands,
receipts, logs, RBFs/reports, independent graph/endpoint comparisons and test
output. Its 31 members are SHA256-verified against `manifest.json` by readback.
Executables are excluded. Archive SHA256:
`aec1b73b46feb0c4c0ece10a1e9563d386bcb01ce59fc604dc5461eeb12f2b1c`.

## One bounded feedback generation

Compiler `687ebf55d2f97baf5c82eebff6b3bc6b2e8bd86e` measures native per-pip
delays in rejected routes and feeds their largest observed delays into one
additional generation. These topology-dependent observations guide search only.
The original fallback timing table is restored before evaluations and final
signoff. Exact wire/pip duplicates are skipped across generations. At most eight
proposals per selected net are measured; the requested full-STA trial budget
is unchanged. All three focused native ranking/guard/rollback tests pass again.

The fresh same-binary baseline/candidate comparison completes. The baseline
again reproduces prior RBF/timing/endpoint bytes and the normalized routed graph.
Generation 0 has 142,358 observed pips; generation 1 has 142,450. Those 92 new
observations produce one distinct additional route for the result net, but its
native worst-sink loss is 1202 ps. Across both generations, all 13 distinct
proposals are worse; none reaches full-design STA and none is accepted.

Final candidate RBF, timing and endpoint reports are byte-identical to its
baseline. Independent graph comparison verifies all 12,605 cells and 15,736 nets
unchanged. All 17,199 matching endpoint rows retain identical setup/hold margins.
The same CPU/microcode/pixel failures and 105 unsupported boundaries remain.
This is a completed negative result, not a timeout or timing qualification.

The feedback pair charges 639.665648 seconds. Total charge in the renewed
allowance is 6554.045138 seconds, leaving **15045.954862 seconds**
(about 4 h 11 min). `feedback-budget.json` is the final cumulative ledger;
`budget.json` preserves the earlier first-generation boundary.
`feedback-capture.tar.gz` has 30 members verified by SHA256 readback against
`feedback-manifest.json`; archive SHA256:
`ad5ca04daed70ed9a7efd44022475924a978cacc71a66dd9d6075ecead4e3bc3`.

These bounded results do not justify a larger route-only search on this fixed
placement. They do not prove no better route exists. The next useful experiment
is coordinated placement and rerouting of the critical CPU result-to-microcode
cone, preserving unrelated routes and retaining the same final timing guards.

The final archive maps original absolute input paths to its `inputs/` members.
Reproduction uses the frozen commands/constraints and compiler identities;
CPU-reference routing is diagnostic and cannot supply a production producer.
No seed sweep or hardware qualification is implied. Draft nextpnr #266 and FES
#979 remain open with timing issue FES #840.
