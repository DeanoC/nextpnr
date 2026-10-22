# PC/XT routed waveform capture, 22 October 2026

One seed-1 route completed and **failed timing**. This is a routed diagnostic,
not a production build, accepted candidate, new core profile or hardware test.
No constraint was relaxed and no timing-failure override was used.

Compiler base `77f3a7b0` was merged with reviewed main `3fea4f9f`, producing
`45476abd07a8881ebeee1e5d061be6b5fe123d71`. This includes main's LUT input timing
correction and GPU-router fixes. The main merge was clean. The placed checkpoint
is the retained #265 baseline: only AD20 reference-profile/load attributes and
its explicit forwarded-clock diagnostic declaration were added. Original
placement, clocks, QSF, seed 1, 100 MHz target, timing weight 2000 and critexp 5
remain unchanged. No new placement was run, so main's placer changes were not
exercised. No synthetic external IO budgets were supplied.

## Final routed result

| Clock | Required MHz | Achieved MHz | Setup WNS ns | Hold WNS ns |
| --- | ---: | ---: | ---: | ---: |
| CPU/system `clocks[0]` | 100 | 85.0123 | -1.763 | 0.774 |
| Memory `clocks[1]` | 100 | 226.6546 | 2.794 | 5.956 |
| Microcode `clocks[2]` | 100 | 101.3250 | 0.085 | 4.655 |
| Pixel `clk_pixel` | 74.2501 | 83.2639 | 1.458 | 0.823 |

The complete final report/inventory are retained from the same route and RBF
execution. There are **489 failing setup rows**, zero failing related-hold rows,
**17,147 endpoint records** and **105 unsupported boundaries**. The inventory
reports `final_analogue=true`, `complete=false`. These counts cannot be compared
to the earlier synthetic-budget host inventory as a coverage regression: that
host scenario enabled other reference models and fictitious IO budgets. No
same-binary paired baseline/candidate comparison was run, so no QoR improvement
or regression is attributed to #266 or the main merge.

The worst path is firmware-loaded/reset control through CGA write logic into
VRAM A1EN, with -1.763 ns slack. It contains 9.231 ns of routing and 1.626 ns of
logic. Its launch net has 1,940 input-pin loads in the saved graph, and later
hops cross the device to the VRAM. The AX-register-to-ALU-result path also fails,
with -1.689 ns slack, 7.636 ns routing and 3.558 ns logic. Its operand and result
routing dominate the short dedicated carry-chain links. Exact paths and segment
sums are retained; no path was cut or given a multicycle exception.

## SDRAM clock result and emitted configuration

The final analyser interval at AD20 DDIO clock ingress is **[1244,1430] ps**,
replacing the earlier placed-graph prediction [6850,6850] ps. The 186 ps spread,
combined with the sampled 30 pF local model, nominal 5 ns source high/low and
explicit zero extra uncertainty, gives conditional lower pulse estimates:

- high: **4154 ps**;
- low: **5064 ps**.

These are conditional routed-model diagnostics. Zero additional uncertainty is
an experiment input, not a measured PLL/buffer property. Source waveform
distortion, qualified fast-corner routing, real load/flight/slew and the actual
SDRAM receiver remain unresolved; `physically_qualified=false` is preserved.
A positive pulse estimate neither repairs nor excuses the internal setup misses.

The emitted RBF's **15 decoded AD20 fields** match all three reference RBFs
(0/15/30 pF): DDR and output/OE selector modes, data/OE delay selectors, local
clock-enable selection, drive/slew/input-standard/pull/hold/clamp settings and
both constant DATAOUT inversions. This discharges the comparison of those local
fields for this artifact only; it is not all-device bit equivalence, an analogue
fast-corner proof or hardware acceptance. The decoder source, binary identity,
reference RBFs and generated tables are retained. Decoder compilation first
needed the generated Mistral header directory added; both logs are retained.

## Execution and validation

The frozen CLI identifies itself as `nextpnr-0.11.1-592-g45476abd`. The binary,
source commit, full argv, selected environment, inputs and outputs are hashed.
The run used `gpu-run` through FES's phase-lease runner, owner `pcxt-agent`,
maximum two GPU leases, `NEXTPNR_REQUIRE_GPU=1`, and a 600-second active timeout.
GPU 0 (AMD Radeon RX 7900 XTX, HIP) was granted automatically. Both GPUs were
free and the queue empty before the run; phase waits were about 0.01 seconds.
The log includes Router1 fallback/cleanup. Record this as GPU plus Router1
fallback evidence, not a pure GPU-only route or a new production producer.

The run returned **1**, retained the failed RBF/reports, and charged **153.451077
seconds** of full wall time including waits. Remaining incident allowance:
**1474.392638 seconds**. No reroll or second PC/XT route was run. There was no kit,
GPU pinning, hardware operation or deployment.

Both waveform tests plus both LUT timing tests passed (4/4). All native-to-
retention tests passed (25/25) after merging main. The portable audit verifies
all retained hashes, exact input delta, waveform arithmetic, configuration-table
equality and rejection by the actual retention guard. It reads final routed
artifacts directly; it does not reload a checkpoint to manufacture signoff.

```sh
python3 docs/validation/2026-10-22-routed-clock-waveform-265/check.py
```

`capture.tar.gz` retains full routed artifacts and both placed inputs, along with
reference RBFs and decoded tables. `run.py` records the exact local replay recipe;
a new run requires a new directory, the recorded binary and a fresh budget check.
The portable checker needs only this committed bundle and Python. The manifest
also records local build/decoder provenance; rebuilding the decoder additionally
needs the identified Mistral headers, generated tables and static library.

## Next work

The next useful closure work is the internal control/data placement: reduce the
large CGA firmware/reset fanout and long VRAM-enable route, and improve locality
from AX through operand selection to the ALU result register. Changes need
functional proof and a paired full-route setup/hold comparison; the present
failure does not justify another seed sweep or lowering the CPU clock. The
microcode half-cycle path has only 85 ps setup margin and must be protected.
Board qualification and the remaining unsupported interfaces are still separate
requirements. FES #840 remains open; this result is not timing acceptance of the
merged diagnostic board work in #836.
