# Experimental capture locality diagnostic

This prototype moves plain capture registers receiving registered data from a
fixed hard IP block. It uses the physical source and sink pins and the existing
timing report to choose the source group. No design signal names or RAM-test
coordinates are built into the pass.

Enable it explicitly through `NEXTPNR_MISTRAL_CAPTURE_LOCALITY`:

```
NEXTPNR_MISTRAL_CAPTURE_LOCALITY="/absolute/path/timing.json 64 24" nextpnr-mistral ...
```

The three fields are a report filename without spaces, a move budget from 1 to
64, and a search radius from 1 to 24. The report must come from the same netlist,
constraints, compiler options and initial placement. Selection checks the
critical capture endpoint's location and incoming source edge. These checks
are not a complete provenance check; callers must retain and compare input
hashes. Run the complete pack/place/route flow so generated PLL constraints are
present. Do not use a packed JSON reload as a clock-matched control.

The pass runs after the existing post-placement passes. It requires an unrouted
design with ordinary placement and rejects active slot regions. Eligible
captures have no control inputs, at most four direct downstream registers on
the same clock and edge, and a registered source whose BEL type has one
physical instance. Protected nets, cells and LABs are excluded. A move uses an
unoccupied, legal FF site with an available feed-through half and partner FF;
it does not evict cells or change logical connectivity. Normal routing may
subsequently insert transparent feed-through buffers.

The score uses physical wire estimates for both incoming and downstream
paths. The prototype checks abstract clock timing, newly failing downstream
setup paths and new or worse hold violations after the group of moves, and
rolls back the group if those checks fail. These estimates do not guarantee
routed timing. Final all-clock setup and hold checks remain mandatory.

## Frozen RAM-test experiment

On the accepted reduction-balancing compiler base `b95ac420`, using the same
frozen synthesis JSON, seed 2, GPU router device 1, placement weight 10,
criticality exponent 2 and enable replication budget 4, the diagnostic selected
64 captures and retained 60 moves. All 71 preceding placement progress metrics
matched the control. Complete routing took 698.13 seconds and produced an RBF
with no final reported hold violations.

| Clock | Control MHz | Prototype MHz | Required MHz |
| --- | ---: | ---: | ---: |
| Memory | 108.944328 | 110.963158 | 130.005203 |
| Pixel | 95.229019 | 95.047997 | 74.250069 |
| Capture | 378.582397 | 307.631805 | 130.005203 |

Memory improved by 2.018829 MHz. Pixel and capture still met their required
clocks, but lost surplus margin. This experiment remained unselected under
the all-clock Fmax nonregression criterion used at the time; it did not close
the 130 MHz memory target. The later
[control-remapping measurement](mistral-control-remapping.md) uses a separately
stated target-clock acceptance rule and discloses reduced positive capture
headroom. That rule does not change this historical experiment's recorded
status. No hardware acceptance is claimed.

The new memory critical path is an error-counter control cone rather than an
HPS data capture. Its 11-literal conjunction uses three LUT levels before the
conditional enable LUT. A generic extension of reduction balancing is the next
mapping target. The placement prototype still needs broader designs, complete
report validation and public option integration before production adoption.
