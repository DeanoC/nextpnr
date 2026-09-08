# Mixed-width TDP hardware acceptance — 2026-09-08

The four retained OSS RBFs passed on the designated Cyclone V kit under owner
`nextpnr-m10k-tdp-mixed`. Each uses one M10K, one HPS GP interface and one PLL.
The board reference and logical port A are 50 MHz; logical port B is 25 MHz.
No non-50 MHz reference is required.

| Artifact | A/B payload widths | Reference reported Fmax | Probe flags |
| --- | --- | --- | --- |
| u10-a2-b1-c0 | 20/10 | 323.834 MHz | `--unit 10 --a-lanes 2 --b-lanes 1` |
| u10-a1-b2-c0 | 10/20 | 406.174 MHz | `--unit 10 --a-lanes 1 --b-lanes 2` |
| u8-a2-b1-c0 | 16/8 | 385.505 MHz | `--unit 8 --a-lanes 2 --b-lanes 1` |
| u8-a1-b2-c0 | 8/16 | 370.233 MHz | `--unit 8 --a-lanes 1 --b-lanes 2` |

All four pass initialization, NEW_DATA on each writer, cross-width readback,
preservation of neighboring lanes after narrow writes, simultaneous disjoint
writes, and clock-enable output hold/write suppression. Sanitized results
are retained in `probe-results.txt`. Each session used kit.py's normal
stop/reboot/release protocol and returned free; no SSH reboot was used.

Timing checks require the 50 MHz reference constraint and explicitly verify
port B's setup and clock-to-output paths, including its 40 ns output period.
These fixtures do not have a separate B-clock register-to-register Fmax
entry. M10K timing reuses the existing backend characterization. Settled-data
kit probes do not establish asynchronous CDC timing or collision behavior.
Shared-clock and raw primitive variants are host-only.

`results.json` records exact uncompressed RBF hashes, source and binary
identities, utilization and host timing results. Extract an image with
`gzip -dc u10-a2-b1-c0.rbf.gz > /tmp/tdp-mixed.rbf` and compare its SHA256 with
the manifest. Generate its probe with the flags above using
`../../true_dual_port_mixed_probe.py`, then load/run under an exclusive kit
session. Source and build commands are in the
[mixed-width TDP guide](../../README-true-dual-port-mixed.md).
