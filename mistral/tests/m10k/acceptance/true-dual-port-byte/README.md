# Byte-masked TDP hardware acceptance — 2026-09-08

These three exact OSS RBFs passed the designated kit probe under owner
`nextpnr-m10k-tdp-byte`. Each uses one M10K, one HPS GP interface and one PLL.
The board reference is 50 MHz; port A runs at 50 MHz and port B at 25 MHz.
No non-50 MHz reference was used.

All three passed initialization, low/high/zero/full masks on both writers,
complete readback through both ports with disabled bytes preserved,
simultaneous writes to different addresses with different masks, and
clock-enable hold/write suppression. The inferred images additionally passed
visible output hold during writes. The direct primitive passed NEW_DATA on
enabled write bytes; disabled write-output bytes were excluded from that
comparison and verified on subsequent reads.

| Artifact | Payload and write output | Reference / port-B reported Fmax | Probe flags |
| --- | --- | --- | --- |
| w20-c0 | Two 10-bit lanes, inferred hold | 259.403 / 331.675 MHz | `--width 20` |
| w16-c0 | Two padded 8-bit bytes, inferred hold | 269.469 / 376.932 MHz | `--width 16` |
| w20-c0-raw | Two 10-bit lanes, native enabled-byte NEW_DATA | 389.408 MHz / output timing path | `--width 20 --raw` |

Reference and B-clock constraints are 50 and 25 MHz, respectively. The raw
fixture has no B-clock register-to-register Fmax entry; its RAM output path
is explicitly checked with a 40 ns period. Timing values reuse existing
M10K characterization. These settled-data diagnostics do not establish CDC
timing, stopped-clock behavior or cross-port same-word collision semantics.
The shared-clock inferred cases are host-only.

`results.json` records uncompressed RBF hashes, source revisions, binary
hashes, utilization and timing. Source and reproduction commands are in
[the byte TDP guide](../../README-true-dual-port-byte.md).

Extract an artifact with `gzip -dc w20-c0.rbf.gz > /tmp/tdp-byte.rbf` and
compare its SHA256 with the manifest. Generate its probe from
`../../true_dual_port_byte_probe.py` using the matching flags in the table.
Load/run under the designated kit's exclusive session protocol, then use
kit.py stop/recovery and release. The inferred and raw probes have different
write-output checks and must match the selected artifact.
