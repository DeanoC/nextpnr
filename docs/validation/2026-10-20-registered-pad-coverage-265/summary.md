# Registered-pad endpoint coverage, nextpnr #265 / draft #266

The endpoint API and routed retention inventory now expose existing qualified
registered-pad STA channels through an explicit opt-in. The frozen PC/XT
conditional diagnostic gains **9 rows across 8 physical pads**; **22 endpoints
remain unsupported**. This is host-only compiler coverage, not an evidenced
physical IO budget, timing closure, route qualification or hardware acceptance.

## Source and immutable binaries

Base: `47c61f2295b96552b58db668a4ec98d4d819e717`.
Implementation: `dd2fee70864e47787678eb4249d4b69c75197ea5` (tree in `manifest.json`).
Driver robustness follow-up: `dcd31ef372858bb919375c5e34f07b6f02885ebe`.
The latter changes only Python driver/tests. Final native binaries were built
from committed `dd2fee70`, version `nextpnr-0.11.1-571-gdd2fee70`, and copied to
`/home/deano/harnesses/pcxt-registered-pad-coverage/final-bin` before testing.
Their SHA-256 identities are in `binary-sha256.txt` and `manifest.json`.

Assigned checkout: `/home/deano/harnesses/pcxt-compiler-repair/nextpnr`, branch
`feat/bounded-placement-swaps`. New local commits only; no push, publication,
merge, FES edit, hardware or additional agent. Parent owns independent audit and
publication. CPU-only Release build uses `ARCH=mistral`, `GPU_ROUTER=OFF`, tests
on, GUI/Python off, IPO on. No binary was tested while being relinked.

## Native contracts and retention

`get_registered_io_endpoints()` exposes identities recorded when native STA
creates each architecture-provided boundary: physical cell/pad, direction,
named model and actual private endpoint. `get_endpoint_clock_pair_timings()`
requires the separate `allow_registered_io=true` plus the appropriate external
launch/capture opt-in. Default callers retain real-port validation. The explicit
path validates native early/late backpointers, current model identity and real
pad-net identity; it does not guess private port names.

Schema **5** adds mandatory `registered_contracts` and `primitive_clocks`, even
when empty. Every pad direction binds its explicit top-level IO constraint,
pad net, placed BEL, cell type, connections, parameters, pin states, attributes
and complete named channel set. Channel metadata retains native setup/hold and
clock-to-Q bounds plus clock ingress and edge. Registered data and OE, SDR/DDR
captures and both constant-forwarder pad edges remain distinct channels.

The existing native primitive-clock checker exposes its already evaluated
period/high/low margins and routed clock interval. Retention requires unchanged
requirements and nonregressing margins, including still-positive margins.
Changed/missing channels, constraints, profile/pin/polarity/model metadata or
clock requirements fail closed. The driver validates endpoint identities and
nested records before indexing them; malformed evidence keeps the exact baseline
bundle. Historical schemas are ineligible. Historical checkers adapt only their
explicit isolated synthetic fixture envelopes, never whole-design inventories.

No timing formula, model coefficient, supported pin, electrical condition,
load guard, constant-enable requirement or primitive-clock limit changed.
Native report arithmetic and physical clock skew supply the rows; a separate
unskewed analyzer supplies path extrema. HPS constant paths and ACLR were not
promoted to static proofs or synchronous approximations.

## Frozen-topology diagnostic

Original checkpoint SHA-256:
`733f1cc8ea765bed46dae44dcd744e4432edd75ddedaf232061c0a1d9756b4a6`.
The original checkpoint, QSF and SDC were not modified. The diagnostic first
reproduces original and prior synthetic snapshots, then adds the separate
`reference-profile-overlay.json` and `registered-synthetic.sdc`. The overlay
explicitly declares the existing RAM Tester reference profile and **unmeasured
30pF load** only on accepted actual placed pins/configurations. External -1..2ns
output bounds are arbitrary **test budgets**, not PC/XT board requirements.

| Capture | Dynamic pairs | Static proofs | Unsupported |
| --- | ---: | ---: | ---: |
| Original graph | 17147 | 1888 | 107 |
| Prior synthetic input/output constraints | 17224 | 1888 | 30 |
| Separate registered-pad profile/load overlay | 17233 | 1888 | 22 |

All previous dynamic rows, all 1888 static proofs and all clocks are identical,
including after the profile overlay. The portable checker compares old evidence
bytes/values rather than relying only on counts. All 69 physical outputs or
bidirectional pads are accounted for in `physical-output-accounting.json`.

Resolved physical outputs are `SDRAM_A[11]`, `SDRAM_A[12]`, `SDRAM_CKE`,
`SDRAM_nCAS`, `SDRAM_nCS`, `SDRAM_nRAS`, `SDRAM_nWE` (one `data` channel each)
and `SDRAM_CLK` (`rise` and `fall` channels). Exact cell/BEL/net/channel/clock
identities and row values are retained in `pcxt-host-diagnostic.json.gz` and
`host-summary.json`.

Remaining gaps:

- **13 registered pads:** `SDRAM_A[0..10]`, `SDRAM_BA[0..1]`. Their real CEOUT
  signals are dynamic, driven via packed ALUT5 nets 60577/60727. The existing
  reference model requires constant enables. `excluded-registered-pads.json`
  records exact cell/pin identities; no enable was forced or silently relaxed.
- **4 reset ACLRs:** require a validated reset-synchronizer/interface contract;
  physical reset qualification remains open.
- **1 HDMI clock boundary:** clock-buffer launch is outside this supported model.
- **4 HPS I2C boundaries:** two constant-zero data channels lack a complete
  external proof; two OE sources lack native launch timing.

`remaining-endpoints.json` records all 22 exact identities, metadata and
connections. There is no matching-unsupported waiver. The original unsafe route
candidate still has **15 newly failing setup pairs**; completing this coverage
cannot make that candidate acceptable. No new Fmax claim is made.

## Validation and reproduction

Final native `IoDelayTest.*:CriticalCohort.*`: **87/87 tests passed** (see `native-final.log`).
Driver: **21/21 tests passed**, covering native acceptance/nonregression,
registered channels, positive clock-margin loss, changed model/pin/polarity,
missing metadata and malformed identities, with byte-exact baseline retention.
The native forwarder fixture retains both polarities and both launch edges,
including negative setup and positive half-cycle hold. Existing native suites
cover SDR/DDR captures, data/OE, missing profile/load/pin/electrical models,
physical clock latency, phase relationships and hard waveform rejection.
The dynamic-CE rejection has an explicit native negative test.

All five prior portable evidence checkers pass, including exact old baseline
artifact identities and the 15 individual setup failures. The new portable
checker verifies the physical/channel accounting, unchanged old rows/proofs/
clocks, native fixtures and evidence hashes. Development captures/logs remain
in the output root. One development test expected zero hold for the falling
forwarder edge; the native half-cycle relationship correctly gives 5000ps.
Only the test expectation was corrected; `focused-dev.log` preserves the failure.

From the repository root (use a fresh output filename for a new capture):

```sh
PCXT_REGISTERED_ROOT=/home/deano/harnesses/pcxt-registered-pad-coverage
"$PCXT_REGISTERED_ROOT/final-bin/nextpnr-mistral-test" --gtest_filter='IoDelayTest.*:CriticalCohort.*'
PYTHONDONTWRITEBYTECODE=1 python3 mistral/tests/routed_swap.py "$PCXT_REGISTERED_ROOT/final-bin/nextpnr-mistral-test"
NEXTPNR_TEST_IO_CHECKPOINT=/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json \
NEXTPNR_TEST_IO_SDC="$PCXT_REGISTERED_ROOT/evidence/synthetic-io.sdc" \
NEXTPNR_TEST_REGISTERED_OVERLAY="$PCXT_REGISTERED_ROOT/evidence/reference-profile-overlay.json" \
NEXTPNR_TEST_REGISTERED_SDC="$PCXT_REGISTERED_ROOT/evidence/registered-synthetic.sdc" \
NEXTPNR_TEST_IO_DIAGNOSTIC=/new/path/registered-host.json \
  "$PCXT_REGISTERED_ROOT/final-bin/nextpnr-mistral-test" \
  --gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints
PYTHONDONTWRITEBYTECODE=1 python3 docs/validation/2026-10-20-registered-pad-coverage-265/check.py
PYTHONDONTWRITEBYTECODE=1 python3 docs/validation/2026-10-20-endpoint-coverage-265/check.py \
  --artifact-root /home/deano/harnesses/pcxt-endpoint-coverage/paired
```

**No route invocation occurred**, including empty CLI routes. Campaign usage
remains **1972.156284888 / 3600 seconds**, leaving **1627.843715112 seconds**.
Coverage is incomplete; no full-design routing is justified yet. Dynamic-enable
model qualification, reset/HPS/HDMI coverage and evidence-backed physical IO
budgets remain open. The 100MHz requirement, routed setup/hold retention,
same-recipe seed gate, fresh producer/exact-artifact qualification and separately
authorized hardware acceptance remain unchanged.
