# Completed analogue observations

`analogue_arc_delay` emits an `AnalogueHop` only after that hop completes.
An empty input waveform cannot contribute a zero observation or calibration
sample. Completed prefixes of failed arcs remain usable; generated, P2P and
NO_DELAY hops can legitimately contribute zero. The collector still retains
the maximum observation per pip across users and repeated calls.

The backend tests cover:

- A real HPS GIN.51.64.18 → H6.46.64.32 first-hop failure through
  `compute_analogue_arcs(true)`, with no observed pip or GIN calibration sample.
- Synthetic connections exercising genuine generated/NO_DELAY/P2P timing modes
  before that failed physical hop. These are mode/collector fixtures, not a
  physically legal routed design. Valid zeros and completed failed-job prefixes
  survive; repeated users/calls neither double-count nor lower existing samples.
- A real configured LAB GIN.1.1.0 → H3.0.1.46 circuit edge. Its parent-compiler
  rise/fall observations (63/83 ps) and quad (60/63/79/83 ps) remain exact.

No cache override semantics, numerical simulation, routing selection, stale-map
cleanup or hardblock waveform model is changed. A fresh full flow is required
for timing comparisons; this change does not erase old entries from a reused
context's observation map.
