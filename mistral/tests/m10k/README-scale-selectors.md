# M10K design-scale selector audit

`scale_selectors.py` routes a caller-provided netlist and checks every packed
`MISTRAL_M10K` in the resulting bitstream. It verifies that a live `CLK2`
reaches `CLKIN.1` and receives the independent-clock selector settings, that
flow-through reads deliberately fan a shared clock to both sinks, and that a
folded constant clock leaves the second sink and independent selectors off.
The audit also checks the TDP selector and explicit output-register settings,
rejects packed constant nets on clock ports, and requires a compressed RBF and
passing timing report.

The fixture stays outside this repository so the check can be reused for a
full application design without adding a generated JSON file. For example,
run it against a Coleco OSS synthesis result with at least 100 M10Ks and 100
independent-clock instances:

```sh
python3 mistral/tests/m10k/scale_selectors.py \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --fixture /path/to/synth.json \
  --qsf /path/to/constraints-oss.qsf \
  --sdc /path/to/clocks-oss.sdc \
  --output /tmp/m10k-scale-selectors \
  --router router1 --seed 7 --freq 74.25 --tmg-ripup \
  --min-m10k 100 --min-dual-clock 100
```

The check is host-only. It does not claim RAM functional behavior, silicon
readback, or Quartus bitstream compatibility; those remain separate
application and hardware tests.
