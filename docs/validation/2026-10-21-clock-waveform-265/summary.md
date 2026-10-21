# Same-corner SDRAM clock waveform reference

The new extractor and evaluator recover useful local pulse bounds without
changing native STA or its acceptance gate. They pair early/late pad paths
at the same load and timing corner and reproduce all 24 generated-clock
pulse checks from three small Quartus fits. This establishes a sampled-load
reference, not PC/XT timing closure or a physical board model.

Base: `dee2b36890255a3c9c9b85ceff74e83095d7c9d0`, branch
`feat/bounded-placement-swaps`, nextpnr #265 / draft #266 and FES #840.
No native binary was rebuilt; source changes are the Python reference tools
and documentation. The reference does not alter the existing independent
pad envelopes, RBF bytes, timing constraints, inventory schema or retention
requirements.

## Result

The inverted constant forwarder is on AD20, 5CSEBA6U23I7, with fitted
3.3-V LVTTL, 16 mA and the existing fixture's default settings. Three fits
use the default zero load and explicit 15/30 pF far-end lumped capacitance.
Each fit includes four slow/fast temperature corners, both clock/pad edges,
and explicit early/late queries. These are reference loads, not Kit A
measurements. No interpolation between them is supported.

The evaluator takes conservative high/low pulse widths **at the DDIO mux
routing ingress** and subtracts worst local pad pulse loss at the selected
load. Because the forwarder is inverted, ingress low maps to output high
and ingress high maps to output low. For an illustrative ideal 5 ns ingress pulse and zero uncertainty:

| Load (pF) | Worst high loss (ns) | Worst low loss (ns) | Output high lower bound (ns) | Output low lower bound (ns) |
| ---: | ---: | ---: | ---: | ---: |
| Default 0 | 0.170 | 0.160 | 4.830 | 4.840 |
| 15 | 0.380 | 0.030 | 4.620 | 4.970 |
| 30 | 0.660 | -0.250 | 4.340 | 5.250 |

A negative low-pulse loss means that edge asymmetry lengthens the low pulse;
it is not extra period. Early/late variation still reduces the combined
lower bound. Losses are rounded upward to 10 ps. The evaluator accepts
explicit upstream bounds and uncertainty; a narrower upstream pulse reduces
the result directly. It never supplies a routed ingress bound itself.

The earlier independent-envelope preflight mixed extrema across corners and
loads. That deliberately conservative model could not establish a 2 ns
pulse. These same-corner observations explain much of that pessimism; they
do not show that the full design or actual board meets its requirements.

## Cross-check and exclusions

The implementation reuses the existing full-path parser and mux-to-pad arc
extractor. It verifies source/report hashes, fitted pad identity/electrical
settings, polarity, launch/pad transitions and four-corner coverage. For each
pulse, the generated-clock report and explicit transfer reports must have
identical nodes, transitions and incremental delays. The mux ingress splits
upstream routing from the local pad path.

The raw output pulse equals the ingress pulse minus local loss. Quartus's
reported width additionally includes its clock-pessimism removal. The audit
checks that arithmetic but records the credit separately and **does not use
it in the model**. That reference route's credit must not transfer to PC/XT.

Quartus 17.0.2 rejected the initial `set_min_pulse_width` query as an unsupported
command. The failed script/log are retained. The successful query adds a
generated clock on DDR_OUT and reports the vendor pulse checks with required
width zero. Positive vendor slack is therefore not a check against an SDRAM
chip's 2 ns requirement. No clock or acceptance constraint was relaxed.
The fixture's pre-existing empty input-delay target warnings and synthetic
output-delay timing misses also remain in the logs; these are characterization
fits, not accepted application routes.

`evaluate()` rejects uncharacterized loads, negative uncertainty and nonfinite
inputs. Every output retains `physical_signoff=false`. Sixteen negative cases
also exercise wrong polarity/load, changed source reports, mixed timing corners,
missing pulse edges, altered mux boundaries, local delays and pessimism credit.
A development negative test initially failed to mutate its selected report;
the final test verifies that the source changes before checking rejection.
Native-binding review also caught an initial evaluator polarity error for
unequal ingress high/low widths. The corrected evaluator swaps those phases;
the regression now checks both asymmetric inputs and every retained raw pulse
report, independently of the illustrative equal-duty-cycle results.

## Reproduction

From a fresh export of this commit, without Quartus or local caches:

```sh
python3 docs/validation/2026-10-21-clock-waveform-265/check.py
```

The archive contains the fitted reference inputs, explicit transfer and pulse
reports, fit logs, failed query and source hashes. The portable checker verifies
the archive and implementation identities, recomputes the model and runs the
positive/negative checks. The existing pad summarizer also accepts all three
new fits independently.

To repeat the three reference fits, use distinct new output directories:

```sh
for load in 0 15 30; do
  timeout --signal=TERM --kill-after=10s 600s python3 \
    mistral/tests/gpio-timing/characterize.py \
    --quartus-bin /path/to/quartus17.0.2/bin \
    --output /new/reference/load${load} --variants clock-forward-inverted \
    --all-transitions --output-load-pf "$load"
done
```

Run the retained `pulse.tcl` in each completed `clock-forward-inverted` project
with `quartus_sta -t`, bounded to 120 seconds. Do not overlap a query with a
fit's automatic STA. Then run `clock_waveform.py` and `check_clock_waveform.py`
as documented in the GPIO fixture README. No seed reroll was used.

## Remaining integration

Native integration still needs explicit binding to the emitted forwarder
configuration and a conservative routed mux-ingress waveform, without
borrowing the reference route or vendor pessimism-removal credit. The native route accessor currently collapses transition delays to a
`DelayPair` (`TimingAnalyser::get_route_delays`); the point-load waveform
reference must not replace the independent `getRegisteredIoTiming` pad arcs.
A future binding must select the AD20 constant DDROUT with `DDR_HIGH=0`,
validate physical/electrical and emitted DQS selectors, explicitly declare a
sampled load (not reinterpret an upper load bound), and keep source-to-buffer
clock distortion separate from buffer-to-mux routing. Its pulse calculation
must swap high/low ingress phases and subtract route spread and uncertainty
before the local loss. Neither the current forwarded-clock inventory, which
only accepts an unregistered AG5 output, nor a nominal 50-percent waveform
establishes that complete AD20 ingress bound. No native gate was changed.

Continuous
load coverage requires evidence beyond these three point samples. Kit A
still lacks an identified chip/speed grade and qualified board load/flight/slew
bounds. HPS peripheral launch/input timing, high-Z turnaround, HDMI and reset
qualification remain open. The whole-design guard stays closed.

Exactly three new bounded primitive fits, zero PC/XT route invocations, no GPU
or hardware use. The PC/XT route allowance remains 1627.843715112 seconds.
