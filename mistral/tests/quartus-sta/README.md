# Quartus STA cross-check of the Mistral timer (#174)

These tools compare nextpnr's Mistral timer with Quartus 17.0.2 TimeQuest on
identical wiring, in both directions. Mistral rnode names are Quartus routing
constraints file (RCF) names under other type names, with the same X, Y and
index:

| Mistral | RCF |
| --- | --- |
| `GIN` | `LE_BUFFER` |
| `GOUT` | `BLOCK_INPUT_MUX` |
| `TD` / `LD` | `LOCAL_INTERCONNECT` / `LOCAL_LINE` |
| `H3` `H6` `H14` | `R3` `R6` `R14` |
| `V2` `V4` `V12` | `C2` `C4` `C12` |
| `WM` | `R14_C12_INTERCONNECT_DRIVER` |

For example, `GIN.029.077.0027` is `LE_BUFFER:X29Y77S0I27`.

- `route_delay.cc` decodes an RBF (Quartus' or nextpnr's) and times every
  routed input port with the per-pip loop of `Arch::analogue_arc_delay`
  (slow 100 °C and −40 °C). It also reports the delay including the sink's
  own block-input stage (`GOUT` → ALM pin). nextpnr's arc to a LUT input
  stops at the `GOUT` rnode and leaves that stage out. Its FF data, FF
  control and M10K arcs end on nextpnr-created wires and keep it.
- `paths.tcl` dumps TimeQuest's worst setup paths, one row per point, for
  every operating corner.
- `compare_paths.py` matches each IC hop of a Quartus path to the decoded
  route ending at its pin. It gives every LUT arc the `delay.cc` per-pin
  delay, so nextpnr's model times Quartus' own placement and routing.
- `critical_path.py` decomposes nextpnr's worst path and checks that
  `route_delay` reproduces every routing delay in the report.
- `replica.py` rebuilds that path as a Quartus project: the same LUT and
  launch-FF sites, the same physical LUT pins, and an RCF holding each hop
  to the decoded rnodes. Each LUT is an inverter of its path input. The
  final hop is replaced by a capture register in the last LUT's ALM, and side
  branches of the original nets are dropped. Decoding the replica's RBF with
  `route_delay` must give the same rnode chain for every hop.
- `replica_compare.py` and `fullpath.py` compare the replica's TimeQuest path
  with nextpnr's report.
- `per_pin.py` writes `per-pin.tsv` from the four-corner arcs retained in
  `../lut6-select`.

## Running

Build against the Mistral tree nextpnr uses (here `8fcc4cb4`):

```sh
c++ -O2 -std=c++17 -I$MISTRAL/libmistral -I$MISTRAL_BUILD/libmistral -I$MISTRAL_BUILD/tools \
    route_delay.cc $MISTRAL_BUILD/libmistral/libmistral.a -llzma -o route_delay
./route_delay 5CSEBA6U23I7 core.rbf routes.tsv
python3 critical_path.py timing.json routes.tsv clk_sys
python3 replica.py routed.json timing.json routes.tsv clk_sys rep
(cd rep && quartus_sh --flow compile top && quartus_sta -t ../paths.tcl top 5)
./route_delay 5CSEBA6U23I7 rep/output_files/top.rbf rep/routes.tsv
python3 replica_compare.py rep
python3 fullpath.py routed.json timing.json clk_sys rep routes.tsv
```

`routed.json`, `timing.json` and `core.rbf` come from one nextpnr run. The
replica needs a nextpnr routed JSON with `NEXTPNR_BEL` and `FES_PINMAP_V1`
cell attributes. `replicas/` keeps the four replicas' Quartus inputs, their
decoded routes and their TimeQuest dumps. `results/` keeps the outputs.

## Inputs used for `results/`

- nextpnr routes: the #218 round-3 replay. Base `9128800f` routes were
  re-timed by `19734dbd`, which has the same LUT pin delays as `main`. Head
  `c9ad2e08` routes were timed by their own build. The JSON sha256 values are
  zx81 s3 base `1047d623…`, head `5f1e9373…`, and sg1000 s2 base `045c3989…`,
  head `96f99135…`.
- Quartus wiring: the FES SG-1000 Quartus oracle project (fes `1d9ba5b5`,
  `scripts/build_fes_sg1000.py --print-commands`, seed 1), compiled with
  Quartus Prime Lite 17.0.2 Build 602. RBF sha256 `db8842dd…`. The FES
  ZX81 Quartus recipe at that commit is missing three sources; with them it
  does not fit (230,139 combinational nodes against 83,820).

## Limits

The replicas cover each path from launch Q to the last LUT output, without
the original nets' side branches. `fullpath.py` keeps nextpnr's skew,
clock-to-Q, final hop and setup, and adds Quartus' missing side-branch load
as Mistral's own difference. TimeQuest's derived clock uncertainty
(170–200 ps here) is left out. All Quartus numbers are its timing models,
not silicon measurements.
