# DSP signedness references

Quartus Prime 17.0.2 Build 602 Lite bitstreams for `5CSEBA6U23I7`, decompiled
with `mistral-cv decomp`, reduced to each DSP block's non-default settings
(clock and clear selects dropped), the DATAIN groups Quartus routed, and the
groups whose nine bits all come from one source (sign extension). They are the
oracle for the signedness configuration `mistral/bitstream.cc` writes;
`mistral/tests/dsp_signedness.py` synthesises `rtl/probe.v` and
`rtl/probe_mixed.v` with a Yosys whose `intel_alm/common/dsp_map.v` forwards
`A_SIGNED`/`B_SIGNED`, routes them, and compares.

## Method

Each module in `quartus/muls*.v` was compiled alone (`quartus_map`,
`quartus_fit`, `quartus_asm`) with `quartus/template.qsf` (device
`5CSEBA6U23I7`, uncompressed RBF, Quartus-chosen pins), then
`mistral-cv decomp 5CSEBA6U23I7 out.rbf out.bt`. `quartus/*.dsp.json` holds
the reduction of each `.bt`. Recorded 2026-10-18 on the kit host.

## Results

| Reference | Design | DSP settings beyond mode | Operand groups |
| --- | --- | --- | --- |
| `mul_u9_q` | unsigned 9x9 | none, `MODE M9X9` | 0, 2 |
| `mul_s9_q` | signed 9x9 | `AX/AY/BX/BY_SIGNED` | 0, 2 plus sign-replicated 1, 3 |
| `mul_m9_q`, `mul_my9_q` | signed by unsigned 9x9 | `AX_SIGNED`, `BX_SIGNED` | signed operand on X, sign-replicated group 1 |
| `mul_u27_q` | unsigned 27x27 | none, `MODE M27X27` | 0, 2, 6, 7, 8, 9 |
| `mul_s27_q` | signed 27x27 | `AX/AY/BX/BY_SIGNED` | same |
| `mul_mx27_q`, `mul_my27_q` | signed by unsigned 27x27 | `AX_SIGNED`, `BX_SIGNED` | signed operand on X |
| `mac_u18_q` | unsigned `A*B+C` | none, `MODE M18X18P36` | 0..3 and C on 6..9 |
| `mac_s18_q` | signed `A*B+C` | `AX_SIGNED`, `BX_SIGNED`, `BY_SIGNED` | plus group 5 bit 8 = sign of Y |
| `mac_sx18_q`, `mac_sy18_q` | one signed 18-bit, one unsigned 17-bit, signed C | `AX_SIGNED`, `BY_SIGNED` | signed operand on Y with group 5 bit 8 |
| `mul_u18_q`, `mul_s18_q`, `mul_m18_q` | plain 18x18 | default mode (`m18x19`); signed Y sets `BX/BY_SIGNED` | 0..3, group 5 bit 8 for a signed Y |
| `mul3_s9_q`, `mul3_u9_q`, `mul2_*18_q` | three 9x9 / two 18x18 | one block per product | Quartus does not pack independent products |

Quartus normalises a signed-by-unsigned product so the signed operand is on X
for 9x9 and 27x27 and on Y for 18x18, so the mixed configurations are not
compared bit for bit; `docs/mistral-dsp.md` describes the backend's rule.
