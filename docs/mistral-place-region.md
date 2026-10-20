# Mistral PLACE_REGION

A QSF instance assignment fences core logic into a rectangle of the device
grid, like a Quartus LogicLock region:

```tcl
set_instance_assignment -name PLACE_REGION "X20 Y0 X89 Y81" -to video.native_capture.capture
set_instance_assignment -name PLACE_REGION "X0 Y0 X89 Y81" -to "video.native_capture.capture.output_pipe*"
```

The value is one rectangle, `X<x0> Y<y0> X<x1> Y<y1>`, in nextpnr grid
coordinates (the numbers in BEL names such as `MISTRAL_FF.30.20.4`), both
corners inclusive. Quartus' multi-rectangle `;` form is rejected.

## Targets and precedence

A plain target names one cell, or every cell below a hierarchy: `-to left`
covers `left.body_MISTRAL_FF_Q` but not `leftover_MISTRAL_FF_Q`. A target with
`*` or `?` is matched against the full cell name, as for I/O assignments.
Names are post-synthesis cell names with `.` hierarchy separators.

Each cell takes its most specific assignment: one naming the cell itself,
then the longest matching target, then the later line in the file. In the
example above the capture's output pipeline matches both lines and takes the
second, whole-device rectangle, which leaves it effectively unconstrained.

Only core logic is fenced: LUTs, flip-flops, MLABs, M10Ks and DSP blocks.
I/O buffers, clock buffers, PLLs and hard blocks named under a fenced
hierarchy keep their own placement.

## Checks

Before placement nextpnr fails with a message naming the assignment when:

- the value is not one rectangle, is empty, or leaves the device grid;
- a target matches no core logic cell;
- the rectangle has no location for a matched cell's type (for example a
  rectangle over the HPS, which holds no LABs);
- a matched cell already belongs to a FES cart region.

After all placement passes, every fenced cell must still be inside its
rectangle; otherwise nextpnr fails rather than route a placement that
broke the constraint. Mistral's post-placement remaps (capture locality,
local, comb and decomposition remaps, critical cohort repair, enable
replication, placed reduction) already leave region-constrained cells alone.

## Why

HeAP weights each arc by 1/fanout, so wide broadcast nets barely pull their
sinks together (#247). On the FES Atari ST the native-capture frame buffer
(about 234 M10Ks fed by broadcast writes) lost to the CPU-side snoop nets
and drifted toward `machine.system` on most seeds. A fence at x >= 20 for
`video.native_capture.capture`, with its output pipeline left free, placed
the capture well on 10 of 10 seeds against 1 of 10 without it. With
congestion-driven HeAP (#250), and the ST's timing fixes, three of four
fenced DSP seeds met both clocks, against one of four unfenced.
