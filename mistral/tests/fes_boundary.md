Issue #165's unused socket FF output is locally vacant, but unrelated frozen
shell nets consume its routes inside the strict CRAM fence. Both CPU and HIP
routers originally reported no path. A different router or an unconstrained
RAM cart cannot repair that scaffold under the existing contract.

`fes_check_boundary_connectivity` runs after LAB pin mapping and global
routing, before selecting a routing backend. It checks each placed cart sink
of a locked shell output using the architecture's reserved mux, private PLL
and fence rules. Locked wire owners and selections remain immutable; movable
routes are ignored. A failure reports the source, a missing sink, the fence,
the number of reachable wires, and frozen net owners blocking the frontier.
Passing is only a necessary connectivity condition, not proof of simultaneous
routability, timing, or complete CRAM containment. A failed placed sink may
require another cart placement; a shell without usable boundary egress must
be rebuilt. No route, BEL, pin map, clock or fence is relaxed by the check.

Run the small ownership, reserved-selection and physical-fence tests with:

```sh
build/nextpnr-mistral-test --gtest_filter='FesBoundaryTest.*:FesCramTest.*'
```

The full seed-4 producer inputs are retained by FES as
`out/dev/atari-st-floppy-geometry/nextpnr-165-seed4-reproducer.tar.gz`
(SHA256 `4be16ffc6c2a85c5f332421184807b05f0668972dbff2e768c57b88353986272`).
They are external to this repository. The frozen PLL pin maps require the
issue's nextpnr revision `3d4a5b352b4edb478b744b82cc61333353751a80` and Mistral
revision `7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039`. Backport the boundary check
to that compiler for a golden replay; current main's revised PLL pin maps and
SDC port lookup reject this old snapshot before routing. Do not rewrite the
golden snapshot to bypass those checks. Extract it to a fixture directory and run:

```sh
python3 mistral/tests/fes_boundary_atari.py --nextpnr /path/to/patched-pinned-nextpnr \
    --fixture /path/to/seed4-inputs --output build/tests/fes-boundary-atari
```

The driver validates all four original input hashes before and after each run.
It retains the full scaffold occupancy, FF34/buffer31 pair, video reservation
24,41..28,58, clock anchors and CRAM rectangle 1769,3442..2806,5162. Both router
selections must reject the boundary before backend routing. This negative
fixture needs no GPU and supplies no successful route, timing or bitstream
qualification. A future shell remedy must separately pass timing, retain the
boundary BELs/clocks and prove zero CRAM changes outside the rectangle.

Validation: the current-main CPU build passed all 16 `FesBoundaryTest` and
`FesCramTest` cases. A scratch backport of the check to the pinned nextpnr,
linked against the original pinned Mistral static library, passed the full
golden replay for both router selections. Each reports the original arc
`WIRE.24.41.FFOUT[22]` to `GOUT.25.42.58`, with 170 reachable nextpnr wires,
three fence-blocked pips and 11 frozen blocking nets including
`video_request[24]`. No backend routing or bitstream generation runs in that
negative case. The 170 wires include nextpnr's synthetic LAB edges, so that
count is not the physical-only graph probe's 46 nodes.
