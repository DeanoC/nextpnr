# Scoped reset, forwarded clock and open-drain contracts — nextpnr #265

The compiler now retains explicit contracts for two reset chains, the AG5 HDMI
clock forwarder and the two constant-low HPS I2C data channels. The frozen
placed diagnostic preserves all **17,246 native timing rows and 1,888 static
proofs**. Only `scl.OE` and `sda.OE` remain unsupported endpoints, but the
inventory is **incomplete**: physical interface obligations remain mandatory.
This is diagnostic/reference coverage, not PC/XT timing closure or board signoff.

Base source: `0937b10a5279ae231b280ee6ced30a118744fd6e`.
Final native source: `0d7db17ecc32dfaf83fea89be526d75d6eeeab5b`.
Source tree, native Mistral identity and immutable binary hashes are in
`source-identity.json`. The existing CPU Release build was reused after checking
its source directory, toolchain, BUILD_TESTS and GPU_ROUTER=OFF configuration.
The binaries were copied to `final-bin`, made nonwritable and hashed before
final tests. No tested binary was relinked. The reused CMake configuration keeps
an old embedded `--version` label (`b484fcbd`); it is not the tested source
identity. The captured clean source/tree identity, incremental build log and
binary hashes identify this run. `CMakeCache.txt` records the reused configuration.

## Reset contract

`timing/reset_contracts` is a JSON array of objects with an ordered `stages`
array. Every chain requires at least two distinct native FFs, explicit per-stage
`async_reg`, common physical clock/edge/reset/polarity, first D constant one
opposite the native reset-zero state, inactive ENA/SCLR/SLOAD overrides, and
sole intermediate-Q-to-next-D fanout. Top-level escapes, cycles, asynchronous
consumers, unknown logic and other clock domains reject. Final fanout is traced
conservatively through combinational logic to native synchronous FF or qualified
registered GPIO consumers. Every interstage D and final consumer must retain
native checked setup **and** hold; cuts or missing rows reject. Raw reset fanout
is enumerated and must stay inside the declared chains.

The source-intent audit independently maps by clock-buffer identity and chain
position. Synthesized names were permuted between runs and were not used to
infer intent. `reset-synthesis.tar.gz` retains baseline/corrected synthesis and
corrected packing. The portable audit verifies that removing only `async_reg`
from corrected synthesis reproduces baseline synthesis. This is not a claim of
whole-design equivalence between current FES and the frozen placed checkpoint.
The original placed file was never edited. `reset-overlay.json` conditionally
adds four audited intent attributes in memory, separately from the original and
registered-pad snapshots. Native validation confirms 554 system-domain and 259
pixel-domain final leaf uses, including the real registered SDRAM I/CEOUT paths.

The contract status is `structurally-validated`, `physically_qualified=false`.
Minimum assertion pulse, PLL lock stability/relock, recovery/removal containment
and metastability/MTBF remain open. No recovery/removal number, false path or
waiver was added; legacy synchronous ACLR timing is not used as that proof.

## HDMI reference and native boundary

Four small Quartus 17.0.2 fits are retained in `reference-fits.tar.gz`: two 0/30pF
reference loads with data D5=0, and two separate selector-control fits with data
D5=31. All use 5CSEBA6U23I7, AG5, 3.3-V LVTTL, 16mA, fast slew, direct global
clock forwarding, all four corners and both transitions with early/late paths.
The load values are declarations, not board measurements.

The native GPIO I boundary is **GOUT.037.000.0000**, mapped through
**HMC.IOINTADDRDOUT.8**. In the fitted route it corresponds to the
BLOCK_INPUT_MUX_PASSTHROUGH ingress into the HMC tail. The supplied tail includes
HMCPHY routing and the complete output buffer/pad, excluding all upstream clock
routing. Maximum observed tails are **6954ps rising / 6481ps falling**;
the outward-rounded reference envelopes are **0..6960 / 0..6490ps**.
The conservative zero lower bound is not a characterized silicon minimum.

Decoded selectors resolve a potentially misleading name: native
`RB_T9_SEL_EREG_CFF_DELAY=31` controls **OE D5**, while data D5 is
`RB_T9_SEL_OREG_DFF_DELAY=0`. The original data-D5=0 fits match these defaults.
The additional D5=31 fits change only the data selector (apart from build ID)
and raise the observed tail maxima to 8134/7660ps. They are retained as
configuration controls, not folded into the accepted reference profile.
No bitstream default or physical configuration was changed.

The narrow `QUARTUS_17_0_2_AG5_CLOCK` profile requires the exact pin/BEL, supported
electrical settings, declared 0..30pF load, default selectors, no inversion,
unregistered OB and direct CLKBUF source. `timing/forwarded_clocks` explicitly
binds top-level output and source clock. The inventory records the source
waveform, source-buffer and upstream-driver configuration, native early/late
route interval, both pad-edge arrival intervals and conservative high/low pulse
bounds. It adds the native route once, without creating a register launch or
inventing a zero external budget. This is a source-relative forwarded-clock
contract, **not automatic downstream SDC generated-clock propagation**.

The placed diagnostic's reference high-pulse lower bound is **-226ps**, and its
low-pulse lower bound is **244ps**. The high-pulse bound does not pass. Receiver
pulse/data relationships, actual board load/flight and a justified fast-corner
clock-route lower bound remain explicit obligations. The retention driver
cannot accept this reference contract as physical qualification.

## HPS channels and retention schema

A narrow constant-channel proof accepts only unregistered IO I=0, including
uninverted identity buffers. It freezes the complete pad/source-chain binding
and rejects inverted I, PAD, BUF.Q or CONST.Q, nonzero values, loops, dynamic or
ambiguous sources. OE is a separate required endpoint. This proof neither times
nor qualifies the whole open-drain pad.

The real HPS `out_clk`/`out_data` OE sources still lack native peripheral-clock
launch bounds. Input feedback/loop behavior, pullup/load and receiver timing
remain open. No fabric system/pixel clock was assigned to HPS.

Schema 6 requires `constant_outputs`, `reset_contracts`, `forwarded_clocks`,
`physical_obligations` and `unsupported_details` as well as the existing metadata.
Missing metadata, pending obligations or reference-only reset/forwarded contracts
reject retention even if `complete` is forged. Existing static/dynamic row and
registered-pad guards remain. Earlier schemas remain ineligible. Historical
portable checkers upgrade only their explicitly isolated host fixtures with
empty new contract fields; historical physical inventories and artifacts were
not promoted or changed.

## Validation and reproduction

Final native `IoDelayTest.*:CriticalCohort.*`: **91/91 passed**.
Retention driver: **24/24 passed**, including contract mutation rejection and
byte-exact baseline artifact retention. All seven prior portable checkers pass,
including the unchanged historical route rejection. Nine independent reference
semantic corruptions reject. Final checkpoint diagnostic passes on the frozen
binary; the parent's independent audit is retained separately.

Portable audit, using committed evidence only:

```sh
python3 docs/validation/2026-10-20-interface-contracts-265/check.py
```

Repeat final native/driver tests with the retained binaries:

```sh
ROOT=/home/deano/harnesses/pcxt-interface-contracts
"$ROOT/final-bin/nextpnr-mistral-test" --gtest_filter='IoDelayTest.*:CriticalCohort.*'
PYTHONDONTWRITEBYTECODE=1 python3 mistral/tests/routed_swap.py "$ROOT/final-bin/nextpnr-mistral-test"
bash "$ROOT/reproduce-host.sh" /new/output.json
```

For reference reproduction, extract `reference-fits.tar.gz` into a new directory
and compile each retained project with Quartus 17.0.2 (`quartus_sh --flow compile
top`), then run `quartus_sta -t paths.tcl`. Rebuild `hdmi_decode.cc` against the
recorded decoder library/generated headers, decode each RBF into `config.tsv`,
and run `reference_check.py` and `reference_negative.py` against that directory.
Raw failures from development and the selector investigation are retained in the
local output root; the immutable final evidence is identified by the manifest.

**Zero PC/XT route invocations**, no GPU, hardware, FES edits, push or merge.
Remaining PC/XT route budget is unchanged at **1627.843715112 seconds**.
Physical budgets/coverage, 100MHz routed setup/hold, the seed gate, fresh producer
and exact-artifact qualification, and authorized hardware acceptance remain open.
