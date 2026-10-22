# PC/XT stationary-branch LUT move experiment

Compiler `66c97e4908304cd46f986a35335cb424cac23864` preserves original
routes to stationary consumers when an ordinary LUT moves. Only the moved
sink branch is reconnected; a moved driver's output is rerouted. Original
physical pin assignments constrain legal target halves, and source/destination
LUT6/carry modes are refreshed transactionally. Existing native clock, endpoint,
setup/hold, legality and rollback guards remain unchanged.

The same-input eight-trial comparison makes every proposal routable, versus
four of eight before this change. It still retains zero moves: six regress a
clock margin; two reach the endpoint guard and reduce positive hold margin
by 15 ps and 24 ps. No rejected candidate becomes a qualified result.

Both final RBFs, routed JSONs, timing and endpoint reports are byte-identical.
The baseline also reproduces the previous four artifacts. Independent checking
covers all 12,605 cells, 15,736 nets and 17,199 endpoint identities. CPU setup
WNS remains -1.638 ns, microcode -0.358 ns and pixel -0.228 ns, with 343 setup
failures and 105 unsupported boundaries. Three focused native tests pass,
including stationary-branch preservation, physical-half legality and mode
restoration. Later physical-RBF oracle tests are committed in `692a1c37`;
their separate log is retained and is not described as a test of commit 66 alone.

Both runs use CPU-reference routing, the frozen placed checkpoint, seed 1,
weight 2000 and exponent 5. The completed baseline takes 256.067201 seconds;
the candidate takes 416.044671 seconds. This adds 672.111872 charged seconds
to the prior 7594.426707 seconds. The user has removed the total route cap;
accounting continues without a remaining allowance. The two completed runs
had 600-second CPU limits. No GPU job or hardware was used in this comparison.

`capture.tar.gz` retains the input bytes, commands, source/binary identity,
logs, reports, RBFs and independent comparison scripts. All 29 archive members
pass SHA256 readback. Archive SHA256:
`f01c6c8946e1b24bd38b71025450bf80d9a0560b967b6ecbb42b8f9a8fe8a295`.
Executables are excluded; rebuild the identified source and substitute its
path and the archived input paths into the captured commands.

This is host diagnostic evidence. FES base is `f09356de`; CPU/system and
microcode remain 100 MHz, pixel remains 74.25 MHz. No RTL, production recipe,
lock, seal or shared contract changes. Timing issue FES #840 and draft PRs
nextpnr #266/FES #979 remain open. Complete timing/coverage, the unchanged
multi-seed gate, a fresh producer and exact-artifact physical qualification
remain required.
