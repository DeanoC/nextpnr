# FES menu initial-routing plateau (nextpnr issue 98)

This fixture retains the synthesis JSON from FES revision
`17929b2bd5636655586e47f121d44242e5519446`, together with the recipe's
`cores/fes-splash/constraints.qsf` and `boards/de10nano/clocks.sdc`.
The synthesis output includes the package build ID; do not regenerate it
when reproducing the original placement. File hashes are in `provenance.json`.

On nextpnr `da1ee82b7b019ff51d62fe41fd9447961f178e1a`, seed 4 reproduces
the issue's iteration sequence and fails after iteration 45 with
`TD.21.5.41` at occupancy 2, reservation -1, and zero arcs unfrozen by the
soft-reservation escape. Initial routing incorrectly took a shortcut
intended for stalled timing repair.

Commit `53e1ad42b3fc3bb016a6dbbddfe3fc292f98b98c` restricts both small-plateau
shortcuts to negotiation attempts that began with frozen timing-repair
arcs. Initial routing keeps its ordinary congestion growth and iteration
budget. No device-kernel change or CPU-router fallback is required for
this reproducer.

Run from the nextpnr source root, using a fresh output directory:

```sh
python3 mistral/tests/gpurouter/menu_ddr_seed1.py \
  --nextpnr build/nextpnr-mistral \
  --fixture mistral/tests/gpurouter/menu_issue98 \
  --seeds 1 4 --repeat 2 --expect-clock endpoint.clk --output /tmp/menu-issue98
```

Use `--seeds 2 3 5` for control routes, or `--gpu-cpu` for the host reference
backend. Each run must complete routing, reach zero overuse, produce nonempty
routed JSON and bitstream files, and pass final analogue timing. Repeats must
match routing checksum, bitstream SHA-256 and timing results. The runner
writes per-run logs and an aggregate `summary.json`.

With nextpnr `655f38334b8a1ba798cc05cf3744b6a897119b5d` (which contains the
policy fix), HIP on an AMD Radeon RX 7900 XTX, the exact retained inputs
pass at 95.26 MHz for seed 1 and 102.11 MHz for seed 4 against 74.25 MHz.
Both seeds reproduce their checksum and bitstream across two runs. These
are compiler regression results; they do not constitute a hardware test.
