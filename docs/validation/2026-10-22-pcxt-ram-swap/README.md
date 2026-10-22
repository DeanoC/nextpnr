# Two reciprocal RAM placement diagnostics

Both independent diagnostic routes completed; neither passes whole-design
timing. Both fix all twelve addresses of the selected microcode ROM and leave
the displaced framebuffer timing positive. Their remaining regressions are
retained below. No hardware operation was performed. The experiments exchange
selected framebuffer M10K sites with microcode ROMs whose address paths have
a 2.5 ns native setup window.

| Input directory | Microcode ROM | Framebuffer partner | Reciprocal locations | Changed bytes |
| --- | --- | --- | --- | ---: |
| `byte3-rom1` | bytes[3].rom.1.0.0 | framebuffer_banks[0].framebuffer.ram.0.54 | M10K (41,42) ↔ (26,40) | 6 |
| `byte2-rom3` | bytes[2].rom.3.0.0 | framebuffer_banks[0].framebuffer.ram.0.52 | M10K (41,34) ↔ (26,41) | 8 |

The exact complete cell names and original occupants are in each `plan.json`.
Both inputs contain two RAM location changes. Each input is independent;
neither includes the other swap, LUT reassociation, decoder relocation or any
FF/keep/carry move.

The source is
`/tmp/pcxt-replicate-leased-20261022/inputs/placed.json`, SHA256
`9241b02074cfee262c985c9ddde7e2c71f05bc654d35b989857a9a93ccc58878`.
The actual route inputs and SHA256 values are:

* `byte3-rom1/candidate-placed.json`:
  `028741a4ab2f017a9b4f7a22f85f58ff80e77d9dcf8f5d0b604de240f519dae8`
* `byte2-rom3/candidate-placed.json`:
  `8e32c76eab0e989c280d6ad73c89c898a1307def82b6158af544f0cc46be1bf6`

`prepare.py` substitutes the two unique BEL tokens in one reciprocal
operation. It parses complete ordered JSON with duplicate-key rejection and
compares the entire graph, including every net alias, parameter, pin-map
attribute, setting and clock. The only differences are the two selected
NEXTPNR_BEL values. Applying the inverse substitution recovers the source
byte for byte. No native JSON writer is used for the route inputs, so the
source's orphan aliases and ordering remain intact. Both candidates retain
all 13,597 whitespace-only ROUTING attributes and no routed LAB snapshot.

Every selected cell is MISTRAL_M10K with strength WEAK=1 and the exact
expected attribute set. Both reciprocal destinations are real native M10K
sites and originally contain the other selected cell. Pin maps are coordinate
independent and remain byte-identical. Each pair has 58 incident nets; every
driver, user and alias, including shared clock fanouts, is retained in its
`affected-net-fanouts.json`.

## Frozen native qualification

The route compiler is the existing frozen
`../reassociate-20261022/nextpnr-mistral`, SHA256
`05dca6ea935a614e4948f5cf9e0fb2b92b3934e9b6891568d13f492d3dbc1162`,
from commit `692a1c372b2b55620ba1fd5e43e0c14ae8ffa4d9`, tree
`9b953cd947a89cbf7da9d203b3e07000783a45c2`. Its embedded build-label string
can be stale; the committed source/tree and binary hash identify it. The
same frozen binary loaded each actual input successfully with
`--no-pack --no-place --no-route`.

`native_ram_verify.cc` is a standalone read-only executable linked against
the already frozen 692 objects and headers under `../ram-decode-20261022/`.
It does not edit shared compiler sources or rebuild the shared compiler.
`build_native.py` verifies every copied object/library hash before linking.
The qualified helper SHA256 is
`a602e00106a42fadbf42c4454e4755ee54b98df9569bd0f9b4975eb2bd03020a`.
Its focused source compiles with warnings treated as errors; linking emits
existing libmistral warnings from the frozen objects.

For each source and candidate load, the helper verifies all 10,448 bound
cells have consistent bindings, valid types and legal native locations. It
checks the native device has 553 M10K sites, all 318 RAM placements are legal,
and every native RAM pin map exactly matches its saved map. The complete
native RAM port timing classifications, clock ports/nets/edges, setup/hold
arcs and clock-to-Q arcs are emitted and compared between loads. Only the
two selected BEL values differ. All RAMs have native WEAK strength and no
keep/location attributes, cluster, relative constraints, region or pseudo-cell
protection after checkpoint reload; there are no active frozen slot/fence
guards or physical routes.

The original checkpoint format does not serialize packer cluster metadata.
This qualification proves the explicit checkpoint diagnostic; it does not
authorize a production pass to ignore live guards. A generic production pass
must independently reject protected, clustered, region-constrained or
otherwise ineligible cells in its actual Context.

`qualify.py` compares complete source and candidate JSON again after loading,
rechecks input/compiler hashes, and rejects four deliberate corruptions per
candidate: a RAM clock parameter, RAM saved pin map, deleted net alias and an
unselected BEL change. The proofs and logs are under each input directory;
`qualification-summary.json` records both successful results. No loading
step serializes or replaces an actual route input.

## Timing rationale and limits

The complete all-318-RAM inventory and 1,104-pair geometry screen are in
`../ram-swap-audit-20261022/`. Thirty-four pairs pass that screen. Each
selected pair retains its full input and return endpoint records in
`timing-screen.json`. The framebuffer partner has 29 active input pins
(19 write-side at 10 ns, 10 read-side at 13.468 ns) and eight output capture
pairs at 13.468 ns. Both sides have related native hold records.

For byte3 ROM1, the model predicts address WNS -841 → +134 ps and 10 → 0
failing address endpoints. The smallest modeled framebuffer input/output
headroom is +2247/+496 ps; the microcode return headroom is +2901 ps.
For byte2 ROM3, the model predicts -1461 → -182 ps and 9 → 3 failures,
with framebuffer input/output headroom +2708/+446 ps and microcode return
headroom +274 ps. Byte2 B1ADDR[6] becomes a modeled new -49 ps failure;
this experiment must be judged by all endpoints rather than only its
original worst bit.

These are geometry estimates against native baseline margins. In particular,
output endpoint margins aggregate launch sources; the screen only subtracts
adverse edge changes and never claims aggregate output improvement from
moving one source. Congestion, actual routing and clock skew can invalidate
the predictions. No setup/hold gain is proven by these inputs or by native
placement legality.

## Completed full-route results

Root ran each candidate with the identical frozen 692 baseline recipe apart
from its qualified input. Both routes wrote complete RBF/routed/endpoint/timing
artifacts and exited 1 for setup failures. Receipt charges include phase-lease
queue time: byte3-rom1 666.1435539578088 seconds; byte2-rom3
699.6301940497942 seconds. These are the two run charges only, not a campaign
total.

`final_checks.py` proves the complete routed graphs match except for each
pair's two RAM BELs: 12,605 cells, 15,736 named nets/alias classes, 19 external
ports, 83,426 logical states and physical pin mappings, and all 2,157 generated
route buffers. Every primitive function, parameter, connection and serialized
LAB field is exact. Eight deliberate graph/pin/alias corruptions reject for
each candidate. Actual routing attributes change on 3,476 nets for byte3-rom1
and on 3,515 nets for byte2-rom3; no routing equality or
emitted-bitstream equivalence is claimed.

All 17,199 native endpoint/clock tuples and every non-endpoint metadata field
are identical. `all-endpoint-deltas.json` retains every numeric difference;
the complete endpoint reports remain authoritative. Setup results are:

| Group | Original failures / WNS (ps) | Byte3 ROM1 swap | Byte2 ROM3 swap |
| --- | ---: | ---: | ---: |
| Whole design | 343 / -1638 | 397 / -2323 | 311 / -1604 |
| CPU same clock | 194 / -1638 | 255 / -2323 | 173 / -1604 |
| All 192 microcode addresses | 147 / -1461 | 141 / -1461 | 136 / -1210 |
| All 90 microcode returns | 1 / -358 | 1 / -1 | 2 / -463 |
| Pixel same clock | 1 / -228 | 0 / +391 | 0 / +286 |

The byte3 ROM1's twelve selected addresses change from 10 failures/-841 ps
to 0/+325 ps. Its two output capture pairs remain positive, worst +2901 to
+3289 ps. The displaced framebuffer's 29 inputs and eight output pairs remain
positive: input worst +2972 to +969 ps, output +1471 to +1676 ps. The whole
design nonetheless gains 119 new setup failures and fixes 65. There are
6,589 setup-margin regressions and 7,507 positive hold-margin decreases.

The byte2 ROM3's twelve selected addresses change from 9 failures/-1461 ps
to 0/+102 ps. Its 29 return capture pairs gain one failure: group worst
+699 to -463 ps. The displaced framebuffer remains positive: input worst
+2533 to +2069 ps, output +1471 to +1095 ps. The whole design gains 56 new
setup failures and fixes 88. There are 6,533 setup-margin regressions and
7,028 positive hold-margin decreases.

Both routes retain zero related hold failures. Minimum related hold is
+793 ps for byte3-rom1 and +795 ps for byte2-rom3, versus +795 ps originally.
The 105 unsupported rows and empty native primitive-clock margin inventory
remain unchanged; they are not waived.

The byte2 return regression is largely on an unchanged decoder-to-capture
connection. The native witness to `eu_op0_select_MISTRAL_FF_Q.DATAIN` still
starts at the moved ROM3's B1DATA[0]. Its ROM-to-select_half decoder edge
grows 2149 to 2529 ps; the decoder at (11,34) to the existing route-through
at (9,35) grows 932 to 2076 ps. That endpoint changes +1110 to -463 ps.
The geometry screen could not bound this downstream reroute. For byte3, the
worst CPU capture becomes dominated by an `eu_register_dx` launch through the
overflow cone. Exact before/after native witnesses are in `path-details.json`;
missing source-specific witnesses are left empty rather than reconstructed.

The local address improvement supports trying the general default-off
window-weighted RAM assignment under independent compiler validation. It does
not make either reciprocal result acceptable. This work changes no RTL, clock
constraint, module contract, keep guard or timing limit. Primitive-clock,
board IO, required seed and exact-artifact hardware gates remain open.

Reproduce preparation and qualification without routing:

```sh
python3 prepare.py
python3 build_native.py
python3 qualify.py
python3 final_checks.py
python3 path_details.py
```

Root owns immutable `route.py` and each `*/route/` path. The preparation,
helper, receipts, complete candidates and complete final route artifacts are
uncommitted evidence under this task output directory. `capture.tar.gz` and
`capture-readback.json` retain and independently hash the complete compact
evidence; compiler executables, objects and frozen source trees are excluded.
