# PC/XT arrival-pin assignment: rejected timing diagnostic

Adding `--arrival-pin-assignment` to the established hard-block-window,
corrected-spreader and 17-literal-overflow recipe worsened final routed timing.
Retain the reference recipe without this option. Neither result qualifies the
100 MHz PC/XT build: both have setup failures and 105 unsupported boundaries.
No hardware ran and no production/sealed artifact was accepted.

Both runs use the same original raw synthesis, constraints, frozen compiler,
seed 1, GPU router, placement weight 2000 and criticality exponent 5. They start
from raw synthesis and perform full packing, placement and routing. The only
candidate option is `--arrival-pin-assignment`. This is a same-execution-model
comparison; the reference is not a reloaded placed/routed checkpoint.

| Final native result | Reference | Arrival-pin candidate |
| --- | ---: | ---: |
| System setup WNS | -434 ps | -497 ps |
| CPU system-clock destination WNS | -173 ps | -358 ps |
| ROM-address setup WNS (2.5 ns window) | +124 ps | -108 ps |
| ROM-return setup WNS (7.5 ns window) | +1257 ps | +259 ps |
| Pixel setup WNS | +2005 ps | +706 ps |
| System / address / return setup failures | 14 / 0 / 0 | 12 / 2 / 0 |
| All setup failures | 14 | 14 |
| Related hold failures | 0 | 0 |
| Minimum related hold margin | +822 ps | +819 ps |
| Endpoint rows / unsupported boundaries | 17199 / 105 | 17199 / 105 |
| Export schema / complete | 7 / false | 7 / false |

Both processes return 1. CPU results above are the system-clock CPU subset,
not whole-system WNS and not the address/return cohorts. The five unrelated
clock-pair rows remain retained; 17194 rows have timed setup and related hold.

`complete-comparison.json` compares every endpoint identity, launch/capture
pair, window and timing flag, with no common-only subset. All identities and
flags match. Every clock, forwarded-clock, IO, output, physical-obligation,
primitive-clock, registered, reset, unregistered and unsupported contract
record matches. Complete constant-output, static-endpoint and unsupported-detail
records also match. Every row's before/after values are retained. Setup improves
on 9067 rows, regresses on 7377 and is unchanged on 750; hold improves on 7931,
regresses on 8231 and is unchanged on 1032. These are complete inventories,
not permission to ignore any regression or unsupported endpoint.

The worst reference CPU path runs from the DS register to BP: routing 6.831 ns
and combinational logic 2.801 ns. The candidate's worst CPU path starts at
`eu_op0_select_MISTRAL_FF_Q_12` and ends at
`eu_tf_debounce_MISTRAL_FF_Q`: routing 7.283 ns and logic 2.506 ns. These are the
actual final report identities; names alone do not establish RTL semantics.
Complete path segments are retained. The detailed report is bounded to the
requested 1024 paths per group; endpoint coverage comes from the full native
endpoint report.

## Complete logical graph accounting

CPU-only fresh pack/place reconstructions use each original route command,
with routing disabled and output paths changed. Their receipts preserve exact
commands and frozen-binary hashes; both return 0. No additional full route
was run for this archive.

`prepared-pair-proof.json` compares all 10448 prepared cells, 13597 named nets
and alias classes, 19 top ports and 60049 connected pin bits. Types, parameters,
connections, logical pin states, physical maps, BELs, metadata and module
attributes match exactly after a bijection established from every named-net
bit. The only setting delta is the explicit arrival-pin option. Three deliberate
corruptions (truth bit, extra alias, input polarity) are rejected.

`routed-proof.json` independently compares each complete prepared graph with
its own complete final routed graph. Each route has 1949 additional transparent
FF route-through helpers. Every helper gets an explicit native-pin/function,
private-output-alias, sole-FF-consumer and physical-half ledger before it is
bypassed for comparison. Helper names or overlapping cell sets are never
presumed equivalent. Every original cell, top port, named net/alias, primitive
parameter and logical input polarity is then checked. Physical pin mappings
change on 5361 reference and 5429 candidate cells. Six deliberately corrupted
final artifacts (helper inversion, private extra alias and LUT truth change in
each run) are rejected.

These proofs establish the stated Boolean/connectivity boundary. They do not
prove arbitrary RTL-to-raw-to-pack sequential equivalence, electrical routes,
RBF bit encoding or the serialized LAB physical encoding. Native final timing
and complete endpoint reports remain independent evidence. Matched unsupported
records do not supply missing physical contracts or board measurements.

## Reproduction and immutable inputs

Compiler result commit: `a361eac060ebc25f9f70ef3bbc126814069d346b`.
Result tree: `5f166732030cdc2a298b6930d837a70e1b7af7a6`.
The build began at parent `1a5a69bb9d5106c8c5a296528bd024126f485983` before
recorded source bytes were committed; source snapshots and build/test logs
are retained. Frozen binary SHA-256:
`e1a121d0de5e496afd96328ed435e77c0b78f913fcfd3768283c01b12313deac`.
The executable is omitted from this compact archive.

Original raw synth SHA-256:
`b4c44b2b66faaf8dd1019c6d8de119c55eecb49856158019ac8899a869de028b`.
QSF SHA-256:
`5eff0d63083685f76041b438c36c18f38b19da98053427c7c2ee155dc3873e1d`.
SDC SHA-256:
`3ba280ad420fee63a546a7c5cda15fc6c480de58cad398c67af2e62d1e296041`.
All three bytes are included. `inputs/original-provenance.json` comes from an
earlier structural experiment and names its earlier compiler; the actual
compiler for these routes is bound by the two route receipts and source identity
above. No later re-synthesis has been substituted as the reference.

The exact commands, environments, leases, return codes and artifacts are in
`reference-baseline/receipt.json` and `arrival/receipt.json`. Original launch
scripts and root analysis remain included verbatim. In the original
`analysis.json`, run key `2` denotes arrival pins, not seed 2. Both use seed 1.
`option-source/` preserves the arrival implementation/tests from the exact
compiler commit; this inclusion is not a claim those tests were rerun here.

After extraction, run `python3 prove-prepared-pair.py`, `python3 prove-routed.py`
and `python3 audit-complete.py` to repeat the graph and complete endpoint checks
without routing. Those scripts find the included reference directory. Original
launch/preparation scripts retain host paths and require the frozen tool and
recorded inputs before use; they are not required for offline proof replay.

`manifest.json` binds every captured member; `collection.json` records original
paths, sizes and hashes. Archive construction and destination readback verify
all members, not samples. Full reference/candidate RBFs, routed JSON, timing,
endpoint reports, logs and receipts are retained, along with both CPU-only
placed reconstructions and proof ledgers.

Only the new arrival route is charged here: **212.69421951379627 seconds**.
The reference route was already charged in its own experiment. Preparations and
proofs add no full-route charge. There is no route-budget cap. This record leaves
100 MHz, coverage, same-recipe seed qualification and physical acceptance gates
unchanged; the next integration step is to keep this option out of the candidate
recipe and continue the separately proved timing work.
