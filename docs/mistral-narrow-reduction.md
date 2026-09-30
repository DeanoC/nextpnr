# Narrow placed-reduction RAM-test measurement

The selected narrow-search profile passed the final V4 native-graph and physical validation. It raises the reported memory Fmax from the accepted groups8 reference's 116.27906799316406 MHz to 116.64527893066406 MHz, a gain of 0.3662109375 MHz. Pixel Fmax declines by 0.5870208740234375 MHz while remaining above its required constraint. Capture Fmax and its 5 ns capture-to-memory window are unchanged.

The comparison is `narrow-search-full-v2-264/route-e0-t0-ac0/comparison-v4.json`, SHA256 `0de965ae99e9708c158b590d62bdcfca31205e821e0b178b5bfb809fefb8918c`. Validator V4 is SHA256 `4905f32ab25b89ff6289c64c5501a9894b83f258229f30cb0156d0948372671f`. It records `graph_proof_available=true`, `passes_practical_timing_gate=true`, and `passes_strict_diagnostic_gate_vs_accepted_g8=false`. The last flag remains false because pixel Fmax decreased. The proof binds 316 artifact hashes and 282 execution-file hashes; preceding validator failures and their records remain preserved.

| Reported clock | Accepted groups8 reference, MHz | Selected narrow-search route, MHz | Required constraint, MHz |
| --- | ---: | ---: | ---: |
| Memory, `ram_clock.clocks[0]` | 116.27906799316406 | 116.64527893066406 | 130.0052032470703 |
| Pixel, `display.pixel_clk` | 93.89671325683594 | 93.3096923828125 | 74.25006866455078 |
| Capture, `ram_clock.clocks[1]` | 307.6318054199219 | 307.6318054199219 | 130.0052032470703 |

This satisfies the user-confirmed practical rule: strictly improve the target-memory Fmax, retain passing non-target constraints and positive required non-target windows, and report no final hold violations. Memory still misses its 130 MHz constraint. This result does not satisfy an all-clock Fmax nonregression rule.

The six reported windows retain their exact clock identities and periods. Rounded values below include the complete reported delay, rather than only a subset of data-path arcs.

| Reported window | Window, ns | Complete delay, ns | Setup headroom, ns |
| --- | ---: | ---: | ---: |
| Capture rising edge → memory falling edge | 5.000 | 2.113 | 2.887 |
| Memory rising edge → memory rising edge | 7.692 | 8.573 | -0.881 |
| Pixel rising edge → pixel rising edge | 13.468 | 10.717 | 2.751 |
| Async → memory rising edge | 13.468 | 3.767 | 9.701 |
| Pixel rising edge → memory rising edge | 13.468 | 4.185 | 9.283 |
| Memory rising edge → pixel rising edge | 7.692 | 4.158 | 3.534 |

Positive headroom is required for the five non-target windows. The negative memory-to-memory headroom is disclosed as the remaining target-constraint miss. Final signoff reports zero hold violations.

The measured compiler is `cc00795f772def091160da354018992600c82869`, corresponding to the three generic commits `8e305cb9`, `6552942d`, and `cc00795f`. Its frozen binary SHA256 is `ae824686382ec40786ac106e523af0e239cc21d355daa116f18fbe8ddcbcbb98`; source identity SHA256 is `882cc97ad05b3af4ce99f5686d7e73d816d2e77c8eb4b9122acf4bdff26cef52`. All 37 focused backend cases passed, including actual future-ALUT2 root-arc ordering and the blocked-original-upgrade/distinct-ALM regression. Both compiler and backend-test builds passed.

The comparison reference is the accepted groups8 full route from source `6ad210a60991f93a607ac4490d6282b1267ec64a`, whose full proof is SHA256 `3fe9d625951ec8d9b9d8034e279d456f82804106761854cefca42a1495830e1e`. Both routes use the same RTL and synthesis inputs, QSF/SDC, BUILD_ID `7168b508ab424b70f1c0f87b2035d821`, device `5CSEBA6U23I7`, seed 2, and GPU device 1. The synthesis JSON SHA256 is `61ee68c657e49364425dd4ba4880a679e0759992f2750e284e256d1e4139de28`.

The selected profile retains three local stages with group budgets 8/1/1, candidate 0, pin optimization disabled and original FF placement preserved; the existing combinational candidate 0; and capture locality with budget 64/radius 24. It then selects freshly qualified eleven-literal candidate 0 at radius 6/default minimum 250 ps, twelve-literal candidate 0 at radius 6/explicit minimum 200 ps, and fresh AC decomposition candidate 0. The selected modeled branch improvements were 1726 ps, 234 ps, and 917 ps respectively. The full routed gain belongs to this combined profile; these modeled values do not establish isolated contributions.

The generic changes recognize three-LUT read-once cubes with 7–12 distinct literals, retaining the existing 11-literal 6/5 partition; accept an optional positive fourth per-stage `MIN_BRANCH_GAIN_PS` token with unchanged 250 ps default; and correct narrow placement ranking using actual future-ALUT2 input delays while preferring distinct ALMs for ALUT6 leaves in each bounded shortlist. The original-site priority, 24-site/16-probe bounds, endpoint/clock/hold/legality checks, and transactional rollback remain required. Leaves with at most five inputs retain their sharing behavior.

The full proof independently closes actual 2048-row and 4096-row selected reductions, the fresh 128-row AC rewrite, retained local/combinational functions, native drivers and consumers, aliases, original physical cell/pin metadata outside declared changes, ALM/LAB state and PLL outputs. Late route-through BUF changes have a declared native ledger and actual source-to-physical-A ancestry proof from a separately bound completed query. No numeric bit is treated as a renamed source merely because its value is reused.

The proof does not reconstruct in-memory indexed-store/free-list/bucket or GPU ID state from JSON. Physical LUT evidence comes from serialized pins and occupancy; RBF/CRAM bits are not independently decoded. RBF generation checks genuine Mistral pips through the same chip database; this is not an independent exported-pip decoder. No hardware acceptance is claimed.

The immutable full record is `narrow-search-full-v2-264/route-e0-t0-ac0/run.json`, SHA256 `02cc5a778ee4c7ee310f0787dfc95176a988077ca1494946d4b2ba30bf61a574`. It completed with exit 0 in 760.067852727836 seconds. Its routed JSON is SHA256 `e3e4c4999bd9113b6eb80021d10b674a518376b2e41a98f880e5fafa4d2cc697`, timing JSON is `fc99943bdf4a9e4cbf9ba5c7cf296439b9d88bcc9a1514fba52ace00b9a43b5d`, and generated RBF is `3bc9174f236a7bb18ed21b0bfec349d03ebfc57925501c830c525d5b000cb462`.

The following diagnostic specifications and command follow that record. `BENCH_FES` and `BENCH_RAW` expand to the recorded input paths; `BENCH_RUN` selects a new output directory. A reproduction needs the retained exact input bytes and freshly qualified ordinals for its compiler and graph.

```sh
BENCH_FES=/home/deano/kepler/worktrees/fes-fes-ramtest-compiler-gains-264-17a49c0d
BENCH_RAW="$BENCH_FES/out/ramtest-wide-reduction-264"
BENCH_RUN=/absolute/path/to/new/output-directory

export NEXTPNR_MISTRAL_CAPTURE_LOCALITY="$BENCH_RAW/route-slots-final/timing.json 64 24"
export NEXTPNR_MISTRAL_PLACED_REDUCTION='ddr0_test.group_valid_MISTRAL_ALUT6_E_Q_MISTRAL_ALUT2_B_Q_MISTRAL_ALUT4_Q 6 0
ddr0_test.group_valid_MISTRAL_ALUT6_E_F_MISTRAL_ALUT5_Q 6 0 200'

cd "$BENCH_FES/sources/misteross"
"$BENCH_RAW/narrow-search-264/nextpnr-mistral" \
  --json "$BENCH_FES/out/ramtest-merged-remap/measured-diagnostic/synth.json" \
  --device 5CSEBA6U23I7 \
  --qsf "$BENCH_FES/sources/misteross/cores/fes-ramtest/constraints.qsf" \
  --sdc "$BENCH_FES/sources/misteross/boards/de10nano/clocks.sdc" \
  --freq 74.25 --seed 2 \
  --placer-heap-timingweight 10 --placer-heap-critexp 2 \
  --replicate-enables 4 --timing-allow-fail --detailed-timing-report \
  --balance-reduction-root ddr0_test.group_valid_MISTRAL_ALUT6_E_Q_MISTRAL_ALUT6_F_E_MISTRAL_ALUT5_Q_1 \
  --remap-plan "$BENCH_RAW/narrow-search-264/local-plan.json" \
  --router gpu --gpu-device 1 \
  --remap-comb-critical "$BENCH_RAW/ready-remap-264/route-g1-c0/timing.json" \
  --remap-comb-candidate 0 --compress-rbf \
  --write "$BENCH_RUN/routed.json" \
  --report "$BENCH_RUN/timing.json" \
  --rbf "$BENCH_RUN/core.rbf" \
  --remap-decompose-critical "$BENCH_RAW/remap-plan-264/route-c0-g1-third-c0-g1/timing.json" \
  --remap-decompose-candidate 0
```

The local-plan reports, in order, are `placed-reduction-root-slack/route-local-r6-s0/timing.json`, `comb-remap-264/route-c0-ready-g4/timing.json`, and `remap-plan-264/route-c0-g1/timing.json`, under `BENCH_RAW`. Their group budgets are 8/1/1; all three select candidate 0 with `optimize_pins=false` and `preserve_ff_placement=true`. Controlled execution and its exact runtime dependencies are recorded in `run.json`; the environment specification above states the two diagnostic inputs, not a replacement for that runtime closure.

The same profile has now been reproduced on merged main `e792486c5763564ffc8a9b56366a11cebd982baa`, including #96's locked-LAB changes and #102's test-only addition, with the three generic commits replayed as source `0c59d78a9d672f5ddc4a81b622055b125f1d2522`. The fresh full GPU route completed in 756.4527281690389 seconds at exactly the same three reported Fmax values and with no final hold violations. All 71 ordinary placement metrics and current selected-stage qualification messages match the accepted run.

The integration comparison, SHA256 `01886d65d6e55f1f1c1d5345e00e3dee5358ce672b0f4a3bac3135f6ec155d4e`, proves exact complete native module identity: 20132 cells, 21870 named nets, numeric bit IDs, routing, aliases, owner export order, parameters, attributes, BELs, full pins and lossless paired PLL outputs. The RBF is byte-identical at SHA256 `3bc9174f236a7bb18ed21b0bfec349d03ebfc57925501c830c525d5b000cb462`. Only the top-level creator/version metadata differs. The comparison binds 339 artifact hashes and 281 execution-file hashes, reusing the accepted physical proof through exact graph/RBF identity. It does not claim fresh separate listing proofs or reconstruct unexported in-memory state.

The combined compiler and test binary build passed, as did 37 focused backend cases, five CLI targets and both #96 host diagnostics (`lab_legalise` and `lab_aclr`). The cold build retains pre-existing narrowing warnings outside the reduction changes. [The integration record](validation/ramtest-narrow-integrated-2026-09-30.json) binds the fresh source archive, binary, host checks, invocation and proof. This is a compiler integration reproduction; FES defaults, selected compiler pins and hardware acceptance remain separate work. The publication commits add documentation only and preserve both measured source identities.

The earlier minimum-200 profile on source `6552942d` completed at 116.130531 MHz memory and was rejected because it regressed the target clock. Its artifacts and result remain preserved under `placed12-minimum-264/route-reduction0-ac0`. The new result does not revise that rejection.

[The machine-readable measurement summary](validation/ramtest-narrow-reduction-2026-09-30.json) records the exact command, local plan, stage specifications, timing and artifact identities. It summarizes the retained evidence; it does not replace the full comparison or bundle the historical raw guide reports.
