# External output endpoint coverage, nextpnr #265 / draft PR #266

Explicitly constrained unregistered OUTPUT captures now use the native endpoint
API. The frozen host-only synthetic diagnostic gains **61 output/OE rows** in
addition to the prior 16 input rows. **30 endpoints remain unsupported**, and the
known unsafe route candidate remains rejected. This is coverage work, not timing
closure, a physical IO constraint recipe, or hardware acceptance.

Native implementation is `16a066248e874c42f16da9e1d85a666915e09ee8`;
a follow-up driver-only change `6df8562f` preserves exact JSON types in output
contract identity (Python otherwise equates boolean false with integer zero).
Both commits are on
`feat/bounded-placement-swaps`, based on the verified clean published HEAD
`5b3e84e7f6add81f3d0e3d81f0e944f06cc4164b`. The assigned checkout is
`/home/deano/harnesses/pcxt-compiler-repair/nextpnr`; all new captures and immutable
binaries are in `/home/deano/harnesses/pcxt-output-coverage`. Only new local commits
were made. No push, publication, merge, hardware, FES edit, or additional agent
was used. Parent owns review and external delivery.

## Implementation and contract

The common endpoint API has a separate opt-in `allow_external_capture` argument,
default false. A capture must have a real connected input terminal, native
endpoint timing class, explicit finite ordered IO delay and matching constrained
capture clock/edge. Both early and late backpointer walks still require supported
launches and finite native arcs. Private registered-pad aliases cannot pass the
real-cell-port check. The Mistral exporter opts in; ordinary callers retain their
previous behavior. External FF ACLR support remains disabled.

No STA propagation, output setup/hold arithmetic, routing, native delay model,
clock phase or clock-skew behavior changed. Existing SDC output maximum consumes
setup budget and minimum controls hold. Tests exercise positive and negative
margins, registered launches through buffers, bidirectional data and OE, launch
clock latency, distinct phase-related external reference clocks and both falling
launch and capture edges. Skewed margins and unskewed path extrema retain their
separate roles.

Schema **4** requires both `io_constraints` and `output_contracts`, even when
empty. Output contracts bind each constrained top-level port to its pad net,
direction, adjacent cell type, complete port/net connections, parameters, pin
states/polarity and attributes. The guard requires exact equality and rejects
missing, duplicate or changed requirements. Old schema-3 complete snapshots are
ineligible: their physical output coverage could be incomplete. Historical
captures are untouched. Only the old evidence checkers' isolated synthetic
fixture envelopes are explicitly adapted to schema 4 for timing-regression
checks; this never promotes historical whole-design coverage.

The exporter also fixes two omissions without changing ordinary timing classes:
unregistered data/OE boundaries are inventoried even with IO timing disabled,
and registered output PAD boundaries are explicitly unsupported. Timing a
fabric-side register input does not prove clock-to-pad capture timing. The latter
adds 21 previously absent SDRAM boundaries, including the clock forwarder. This
is why the new original-graph unsupported count is 107 rather than the historical
20; all prior identities and timing rows remain present.

## Frozen graph accounting

The original placed checkpoint SHA-256 remains
`733f1cc8ea765bed46dae44dcd744e4432edd75ddedaf232061c0a1d9756b4a6`.
The diagnostic imports it without routing or bitgen and applies only the
separate `synthetic-io.sdc`. Its arbitrary input 1..3 ns and output -1..2 ns
values are **test budgets only**, never physical constraints. SDRAM test captures
reference `clocks[0]`; HDMI references `clk_pixel`. Input launches retain falling
`clocks[0]` and rising `clocks[1]` captures. The existing pin assignments,
registered-pad profiles, loads and source graph are unchanged.

| Inventory | Dynamic pairs | Static proofs | Unsupported |
| --- | ---: | ---: | ---: |
| Historical original export | 17147 | 1888 | 20 |
| Historical input-only synthetic export | 17163 | 1888 | 70 |
| Current unchanged original graph | 17147 | 1888 | 107 |
| Current synthetic input + output graph | 17224 | 1888 | 30 |

The 107 are the original 20 plus 66 unregistered output/OE boundaries and 21
registered PAD boundaries. Synthetic budgets resolve 16 input and 61 output
endpoints, with no disappearance of other endpoints. All original rows and all
prior synthetic input rows are identical, as are all clocks and 1888 static
proofs. All 69 physical output/bidirectional pads are accounted for in
`physical-output-accounting.json`.

| Output class | Dynamic endpoints resolved | Remaining unsupported |
| --- | ---: | ---: |
| SDRAM DQ data / OE | 16 / 16 | 0 |
| SDRAM DQML / DQMH | 2 | 0 |
| HDMI data / VS / HS / DE | 27 | 0 |
| Registered SDRAM address / bank / commands / CKE | 0 external-pad rows | 20 PAD boundaries |
| SDRAM DDR clock forwarder | 0 | 1 PAD boundary |
| HDMI clock forwarder | 0 | 1 I boundary |
| HPS I2C constant data / OE | 0 | 2 I + 2 OE boundaries |
| PLL reset ACLR | unchanged | 4 |

The 20 registered SDRAM data/control pads are 13 address, two bank, four command
and one CKE. Their fabric-side endpoints remain accounted for as before. No
qualified external load/profile was invented for them. CKE and nCS are not classified as constant outputs: their packed I terminals
are driven by reset/control LUT logic. Constant control pins, where present, do
not substitute for the registered pad/enable and external capture contract. SDRAM DDR clock forwarding also needs
its characterized pad and waveform relationship. The HDMI clock is driven from
a clock buffer, not a registered data launch. These classes remain unsupported
rather than being approximated through private STA aliases.

HPS I2C I pins are direct constant-zero sources. Their OE pins originate at
`cyclonev_hps_interface_peripheral_i2c.out_clk/out_data`, with no supported native
launch timing. No generic constant-output proof was added: lack of a data
transition does not discharge the associated open-drain enable and external
capture obligations. Complete constrained output connectivity/polarity metadata
is retained, and both channels remain explicit unsupported entries. There is no
matching-unsupported waiver. `remaining-endpoints.json` retains identities,
parameters, attributes and source traces for all 30 gaps.

Reset modeling is unchanged and owned by the parent. The four PLL-derived ACLRs
are not legalized by synchronous data budgets. The historical candidate still
has 15 newly failing setup pairs, 7135 setup decreases and 6181 hold decreases;
completing IO coverage alone cannot qualify it. The exact old baseline bundle is
retained and checked again. Historical 89.581650 -> 91.726288 MHz is not 100 MHz
closure.

## Validation and restart

Final immutable binaries are `final-bin/nextpnr-mistral{,-test}`, version
`nextpnr-0.11.1-567-g16a06624`. `manifest.json` records full source, tree,
binary, input and evidence hashes. CPU-only Release build: `ARCH=mistral`,
`GPU_ROUTER=OFF`, tests enabled, GUI/Python disabled. Tests never ran against a
binary while it was being relinked. One development native test failed because
it tried to add a second SDC reference clock; the fixture now replaces explicit
saved constraint metadata. Its failed log is retained.

Final checks and logs (the driver follow-up is in `driver-contract-types.log`;
C++ source and immutable binaries remain identical to `16a06624`):

- Native `IoDelayTest.*:CriticalCohort.*`: 86/86 passed (see `native-final.log`).
- Driver: 19/19 passed, native safe relative gain, setup/hold regression,
  clock/edge/min/max/direction mutations, missing/duplicate constraints,
  incomplete/changed output metadata, missing OE and byte-exact baseline retention.
- Host-only frozen diagnostic: passed; neither inventory is final analogue.
- CLI default/zero-budget baseline export: passed, two empty CPU routes.
- All four earlier portable evidence checks passed, including full original
  endpoint/baseline artifact hashes and all 15 individual setup failures.
- New portable checker validates the complete accounting, native output fixtures,
  old row/proof/clock identity and evidence hashes.

No full-design route or GPU work was performed. The two empty CLI routes were
reserved for at most 60 seconds before launch and charged for the entire
10.001284888-second invocation. Campaign total is **1972.156284888 / 3600 seconds**,
leaving **1627.843715112 seconds**. Every invocation is terminal. No routing was
justified while physical IO/reset coverage remains incomplete.

From the repository root, with a new output directory for generated captures:

```sh
/home/deano/harnesses/pcxt-output-coverage/final-bin/nextpnr-mistral-test \
  --gtest_filter='IoDelayTest.*:CriticalCohort.*'
PYTHONDONTWRITEBYTECODE=1 python3 mistral/tests/routed_swap.py \
  /home/deano/harnesses/pcxt-output-coverage/final-bin/nextpnr-mistral-test
NEXTPNR_TEST_IO_CHECKPOINT=/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json \
NEXTPNR_TEST_IO_SDC=/home/deano/harnesses/pcxt-output-coverage/synthetic-io.sdc \
NEXTPNR_TEST_IO_DIAGNOSTIC=/new/path/host.json \
  /home/deano/harnesses/pcxt-output-coverage/final-bin/nextpnr-mistral-test \
  --gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints
PYTHONDONTWRITEBYTECODE=1 python3 docs/validation/2026-10-20-output-coverage-265/check.py
PYTHONDONTWRITEBYTECODE=1 python3 docs/validation/2026-10-20-endpoint-coverage-265/check.py \
  --artifact-root /home/deano/harnesses/pcxt-endpoint-coverage/paired
```

Next work is a sound registered-pad/forwarded-clock and HPS interface contract,
plus the separately owned reset contract. Evidence-backed physical SDRAM and
HDMI requirements remain necessary. Do not start another full-design guarded
route until coverage is trustworthy; then address the known dynamic regressions.
The unchanged 100 MHz requirement, per-endpoint setup/hold nonregression, minimum
gain, same-recipe seed qualification, fresh producer identity, exact-artifact
qualification and separately authorized hardware acceptance remain mandatory.
The inherited unrelated LAB legality and reload/route timing issues are not fixed
or requalified here.
