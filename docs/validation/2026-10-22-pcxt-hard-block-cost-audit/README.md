# PC/XT hard-block cost audit

The current assignment objective itself places the two failing display RAMs far from their read logic. Their solved RAM/read-logic targets are already adjacent: CGA near (14.15, 13.54), MDA near (16.22, 14.69). At the selected iteration 10, native assignment instead chooses CGA (14,42) and MDA (69,16). Subsequent logic movement does not originate that separation. All twelve tested reciprocal swaps with six nearby framebuffer sites increase the existing complete objective: CGA by 0.678–1.585 million units and MDA by 1.371–3.841 million units.

The independent reconstruction covers all 318 RAMs, 553 native sites and 9,732 timed incident edges. It reproduces all 318 assigned BELs. The original nine-digit coordinate trace gives objective 2,741,096,631; the new seventeen-digit native ledger gives 2,741,096,630. This one-unit rounding difference changes no assignments.

The logging-only compiler change is a4dabe7c, built from source/tree recorded at ffbc638c. Binary SHA256 is `ab3b356b1aa0fd6286dc7c1cc7eba814322c63b99dddb723363e13911e32c31b`. The cached build label is stale; exact source and binary hashes are authoritative. Default, old-policy debug off, and old-policy debug on each reproduce the old checkpoint bytes exactly. All 15 debug iterations contain the complete 318 targets and 9,732 edges; exact sink/anchor identities, positive fanouts and window weights match the independent logical/native inventory. Logging adds no timing run, ID interning, RNG call or policy change.

Ordinary HeAP already divides each arc by its net fanout and multiplies by `1 + timingWeight * pow(nativeCriticality, criticalityExponent)`. The native analyser is setup-only; iteration 10 uses timing last updated after legal iteration 9. Criticality is a relative ranking within each clock-domain pair, so the existing native `1/window` factor must remain to distinguish the 2.5 ns capture window. The measured recipe uses timing weight 2000 and exponent 5.

| Replayed objective at fixed iteration-10 anchors | CGA site | MDA site | Changed RAM assignments |
| --- | --- | --- | ---: |
| Existing windows | (14,42) | (69,16) | 0 |
| Windows and fanout | (26,36) | (38,27) | 312 |
| Windows and native criticality | (26,33) | (41,18) | 314 |
| Windows, fanout and native criticality | (14,40) | (41,25) | 316 |

The combined objective falls from 196,786,381,597 at the old matching to 149,500,837,300 at its own optimum. All sixteen 2.5 ns RAMs stay in columns 5 and 14. CGA's summed input/output weights change from 183,512/6,554 to 332,173/5,056,965; MDA changes from 163,850/13,108 to 1,007,521/7,688,866. Wider framebuffer reads still matter: example framebuffer `.ram.0.8` has eight read branches totalling 13,823,879 proposed weight. The combined rule therefore does not guarantee a nearby CGA site or timing closure.

The smallest justified experiment is a separate default-off option applying those existing HeAP factors to every validated hard-block edge. Retain the current clock-window factor, complete input/output cone checks, all bucket eligibility rules, bounded deterministic assignment, and all-or-nothing native legality fallback. Preserve fractional weights until rounding the complete site cost; reject nonpositive fanout, nonfinite or out-of-range criticality/configuration, and cost overflow. No new per-core priority, forced site, timing exception or remaining-budget model is justified by this audit.

This is a fixed-anchor objective replay, not native placement of the changed objective. A real change affects later timing, solved anchors, best-iteration selection and refinement. No replayed matching proves native placement legality, routed setup/hold, physical LUT masks, RBF correctness or hardware acceptance. The compiler implementation and its later evidence are kept separately in `out/hard-block-timing-20261022`.

A later compiled C++ helper check, retained under `out/hard-block-timing-20261022/exact-native-replay.json`, matches all 97,320 actual candidate edge weights exactly. Its original-snapshot replay gives the authoritative objective values above. The initial `analysis.json` used rounded Python `pow`, differing by five units in the optimum and seventeen in the old-matching objective; all 318 resulting assignments are identical. Both original and corrected evidence remain preserved. This numerical clarification changes no placement or timing conclusion.
