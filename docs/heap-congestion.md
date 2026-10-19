# Congestion-driven HeAP spreading

HeAP minimises wirelength and spreads cells until no location holds more
cells than it has BELs. Nothing in that loop knows about routing, so a dense
region is packed as tightly as the BELs allow and the router finds out
afterwards. On the FES Atari ST core (#247) the router's overuse then
plateaus at thousands of wires and most seeds never route.

With `placerHeap/congestion` set, HeAP estimates routing demand after every
legalisation and makes the spreader treat congested locations as smaller:

1. **Demand (RUDY).** Every net spreads its estimated wirelength evenly over
   its bounding box: `q(p) * (w + h - 1) / (w * h)` per location, where `w`
   and `h` are the box width and height in tiles and `q(p)` is the expected
   number of box crossings of a `p`-pin net (Cheng, "RISA", ICCAD 1994), so
   high-fanout nets are not under-counted as HPWL does. Nets driven from
   global buffers are skipped, as they are for HPWL.
2. **Capacity scale.** Where the demand `d` exceeds
   `placerHeap/congestionTarget`, the location's scale moves halfway towards
   `max(congestionMinScale, target / d)`. Averaging with the previous scale
   stops one iteration's legalisation from swinging it back and forth.
3. **Spreading.** The cut spreader multiplies the BEL count of each location
   with at least four BELs of a type (logic, not single RAM or DSP blocks) by
   that scale. Over-full regions grow out of congested areas sooner and their
   cells are spread over more of the device. Strict legalisation still uses
   the real BELs.

A spreader run ignores the scale when any cell type's effective capacity
would hold fewer than 105% of its cells. The check applies the same beta
threshold as overuse detection, including its small-capacity exception. It
also ignores congestion scaling when a movable cell has a BEL-constrained
region: device-wide capacity cannot show that the cell's own region has enough
scaled BELs.

| Setting | Default | Meaning |
|---|---|---|
| `placerHeap/congestion` | off (Mistral: on) | enable congestion-driven spreading |
| `placerHeap/congestionTarget` | 100 | RUDY demand (wire-tiles per tile) above which capacity is scaled down |
| `placerHeap/congestionMinScale` | 0.5 | smallest capacity scale of a location |

Mistral turns it on, except for FES carts placed into their reserved
rectangle, which have no spare capacity to spread into. Pass
`--set placerHeap/congestion=0` for the previous behaviour; with it off every
cell lands on the same BEL as before this change.

After each HeAP iteration the placer logs the number of locations over
target and the peak demand of that iteration's legal placement (ST, seed 1):

```
Info:     at iteration #1, type ALL: wirelen solved = 94828, spread = 599949, legal = 757627; time = 4.63s
Info:         congestion: 2494 locations over target 100.0, peak demand 414.1
Info:     at iteration #2, type ALL: wirelen solved = 104372, spread = 604093, legal = 684089; time = 2.00s
Info:         congestion: 2864 locations over target 100.0, peak demand 361.9
```

## Choosing the target

On the ST placement of seed 4, the share of tiles that still held an
overused wire after 23 router iterations rose steadily with RUDY demand:
5% below 20, 39% at 45 to 60, 60% at 80 to 100 and 82% above 200. There is
no sharp knee, so the target trades routability against the critical-path
delay that spreading adds to designs that would have routed anyway. Target
45 with minimum scale 0.3 cost the small FES cores up to 1.8 ns of signoff
slack (Spectrum) and failed SMS seed 2. Target 100 with scale 0.5, together
with the GPU router's chunked overflow batches, routed all six ST seeds tried
(1, 2, 3, 4, 5, 7) in two minutes or less and left C64, Coleco and SMS
roughly where they were. Spectrum lost 0.6 and 1.1 ns of slack on its two
seeds; seed 2 now misses signoff by 14 ps.
