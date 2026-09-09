# True dual-port hardware acceptance — 2026-09-08

Both retained native-width OSS images passed the generated TDP probe on the
designated DE10-Nano kit under owner `nextpnr-m10k-tdp`. Each uses one M10K,
one HPS GP interface and one PLL. The board reference is 50 MHz; port A runs
at 50 MHz and port B at 25 MHz. No non-50 MHz reference was used.

The checks passed for initialization, each writer, own-port NEW_DATA,
opposite-port readback, simultaneous writes to different addresses, and
clock-enable suppression of output changes and writes. They do not establish
cross-port collision behavior, clock-stop behavior or CDC timing acceptance.

| Artifact | Reported reference-clock Fmax | Constraint |
| --- | --- | --- |
| w10-c0 | 359.195 MHz | 50 MHz |
| w20-c0 | 301.386 MHz | 50 MHz |

Host checks additionally cover padded 8/16-bit payloads and native widths
sharing a clock. Those four images have not been tested on the kit. Host
reports verify the separate B-port output clock with a 40 ns period and B-port
input setup paths. The timing model reuses existing M10K timing values.

`results.json` records exact source revisions, binary and uncompressed-RBF
hashes, utilization and timing summaries. The source and generated probe are
`../../true_dual_port.v` and `../../true_dual_port_probe.py`; regenerate host
artifacts with `../../true_dual_port.py`. Quartus configuration references are
retained separately under `../../oracle/tdp10` and `../../oracle/tdp20`.

```sh
gzip -dc w20-c0.rbf.gz > /tmp/tdp20.rbf
sha256sum /tmp/tdp20.rbf
python3 ../../true_dual_port_probe.py --width 20 > /tmp/tdp20-probe.sh
```

Compare the digest with `results.json`, claim the designated kit using its
existing session protocol, load the RBF and run the generated shell probe on
the target. Substitute 10 for the other artifact. Stop/recovery and lease
release use kit.py; no SSH reboot or direct programming bypass is needed.
