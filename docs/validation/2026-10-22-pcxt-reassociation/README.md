# PC/XT exact two-LUT factor reassociation

Compiler `692a1c372b2b55620ba1fd5e43e0c14ae8ffa4d9` adds an explicit,
default-off diagnostic rewrite of two ordinary LUTs. A factor from a producer
moves directly to its exclusive consumer, shortening that factor's serial path.
The pure helper exhaustively proves the consumer output for up to eleven
independent signals, including aliases, inversions and constants. The Context
pass preserves original consumer slots and legal placement, with exact rollback
on rejection or exception. The internal producer output may change and must
have exactly one ordinary consumer and no top-level observer.

The CLI requires explicit `--reassociate-producer` and `--reassociate-input`, an
unrouted placed checkpoint, `--no-pack --no-place`, and an ordinary design.
Protected cells, routed inputs, fixed or fenced designs and incompatible repair
passes are rejected. The expanded consumer either stays legally placed or uses
a nearby empty whole ALM. This diagnostic transformation proves Boolean
preservation; it does not invoke or bypass the routed-retention acceptance guard.
A full route remains mandatory and may fail.

## First complete comparison

Both runs use compiler 692, the same frozen placement, QSF/SDC, seed 1, weight
2000 and exponent 5. Routing is authenticated HIP through the shared phase-lease
wrapper. CPU/system and microcode stay at 100 MHz; pixel stays at 74.25 MHz.
The baseline reproduces prior CPU-reference RBF/timing/endpoint bytes.

The selected `idiv_opcode...ALUT6_Q` producer input A is promoted into its LUT5
consumer. The pair becomes LUT5/LUT6 and the consumer stays at MCOMB.8.37.13.

| Native final metric | Baseline | Reassociated |
| --- | ---: | ---: |
| CPU/system setup WNS | -1.638 ns | -1.301 ns |
| Microcode return setup WNS | -0.358 ns | -0.002 ns |
| Pixel setup WNS | -0.228 ns | +0.012 ns |
| CPU-to-ROM address setup WNS | -1.461 ns | -1.189 ns |
| Failing setup rows | 343 | 295 |
| Failing related hold rows | 0 | 0 |
| Unsupported boundaries | 105 | 105 |

All 17,199 endpoint identities match. Full rerouting improves 7907 individual
setup margins and worsens 5327; 6721 positive related-hold margins decrease.
The candidate therefore does not establish timing closure or safe retention.
The rewritten pair disappears from the saved maximum-endpoint paths; these
reports do not establish its exact new input arrival or isolated delay gain.
The new worst CPU branch is an overflow reduction from the DX register.

## Validation and proof boundaries

Nine pure and seven Context tests pass; the CLI boundary regression passes.
Seventeen physical-RBF/pure-helper tests also pass, including eight independent
physical LUT-mask tests. The nine pure tests pass ASan/UBSan with leak detection
disabled; LeakSanitizer cannot run under the harness ptrace environment. Logs
retain these distinct test invocations and source/build provenance.

Independent final routed-JSON proof accounts for all 12,605 cells, 15,736 named
nets/aliases and 2157 generated route-through buffers. All 12,603 outside-pair
cells retain their functions, connections, attributes and 83,413 decoded pin
states. Consumer Q is equal for all 1024 assignments over ten external signals.
Only the two expected serialized ALM LUT6-mode changes occur. Seven deliberate
mutations are rejected. This establishes complete Boolean/structural preservation
of the routed graph, not physical RBF encoding or hardware acceptance.

The normal reader/export drops eighteen existing names from the raw placed
input. The placed proof therefore compares an unmodified same-binary export
against the transformed export. Final proof compares complete final routed
inventories, without intersecting or collapsing them. Both proof inputs and the
checks are retained.

The 105 unsupported boundaries have no IO constraints/contracts in this capture.
`coverage-next.md` identifies implemented diagnostic models and outstanding
qualification work. Existing diagnostic contracts cannot be re-labelled as
measured board margins; native primitive-clock margins are also not established
by this snapshot. Kit A/B physical evidence remains separate.

## Evidence and accounting

The baseline takes 151.901067 seconds and candidate 155.174748 seconds, adding
307.075815 seconds. Cumulative renewed accounting is 8573.614393 seconds through
this comparison. The user removed the total route cap; native search bounds
and shared GPU leases remain in force. No external GPU-job timeout is used.

The capture retains exact commands, inputs, compiler commit/tree/binary hashes,
logs, reports, RBFs and independent proofs. Executables are excluded; rebuild
source 692 and substitute archived input paths into the captured commands.
Archive identity and readback verification are appended below.

FES base is `f09356de`. This is host diagnostic evidence only; no hardware,
RTL, production recipe, lock, seal or shared contract changes. FES #840 and
both draft PRs (#979 and nextpnr #266) remain open. Complete routed timing and
coverage, the unchanged multi-seed gate, a fresh producer and exact-artifact
physical qualification are still required.

Archive SHA256: `0511f300a4ece620dfb2323b3de7fc3d4f72e6787aae0da2b45b1fb4958f20cc`;
42 members pass SHA256 readback.

## Cumulative factor follow-up

Two further exact rewrites target the newly critical overflow reduction. Step 1
promotes the middle LUT's E input, relocating its expanded consumer from
COMB.9.36.31 to COMB.9.38.30. Step 2 promotes the appended F through the next
LUT. Both retain the first proved rewrite.

A same-binary baseline reload reproduces first-factor RBF/timing/endpoint bytes
exactly. The complete comparisons are negative: step 1 has CPU WNS -1.323 ns,
microcode -0.548 ns, pixel -0.554 ns and 336 setup failures; step 2 has CPU WNS
-1.524 ns, microcode -0.038 ns, pixel +0.921 ns and 318 failures. Neither has
related hold failures; both retain 105 unsupported boundaries. The critical
input switches to another branch of the same reduction. This motivates balancing
the entire private tree instead of promoting one factor at a time.

Independent complete-graph proofs cover 512 and 1024 assignments. The generated
DATAIN route-through introduced by the step-1 relocation is explicitly proved
by identity, private net, sole user, function and physical half; other generated
helpers are also accounted for. No intersected-graph proof is used. Eighteen
negative mutations reject. These remain Boolean/structural diagnostics.

Baseline/step1/step2 charge 155.126424/201.845970/198.364968 seconds, totalling
555.337362 seconds. Including the intervening two-ROM experiment, cumulative
renewed accounting reaches 9312.848848 seconds; the user-imposed total cap
remains removed. No hardware or production settings change.

`chain-capture.tar.gz` has 38 SHA256-verified members, archive SHA256
`316af6596517d5a0dfc66a15831f8ce782dba706ab4728576492c08e1c91b1ce`.
It uses the frozen compiler and first-factor input from `capture.tar.gz`.
