# Isolated post-placement enable-locality probe

Diagnostic branch `probe/ramtest-enable-locality`, based on
nextpnr `19a5fe4a`. The hook changes `mistral/arch.cc`; this directory contains its snapshot checker.
This branch is a fixture-specific experiment, not a production optimization.

The frozen executable is `tools/nextpnr-enable-locality`. Enable the probe with:

```sh
NEXTPNR_MISTRAL_ENABLE_LOCALITY=/absolute/existing/directory/probe \
  /path/to/nextpnr-enable-locality <ordinary RAM-test arguments>
```

With the variable absent, the hook performs no changes or identifier
allocations. The entire placed module of the existing 197-cell regression
fixture is identical to the pre-probe compiler's output in disabled mode.

The hook runs after ordinary HeAP placement, before LAB routing preparation.
It requires the exact port-1 enable ALUT3, mask 0x32 and 111 ENA sinks. It
clones that LUT with identical parameters, input nets and folded pin states.
All users of this enable in LABs (34,22), (34,25), (35,25) move together onto
the clone. The hook and checker require exactly five such sinks; selection is by entire LAB,
not by register name. No original cell moves, no registers are replicated,
and no logic depth or register initialization changes.

The clone uses a free legal combinational BEL within x=30..39, y=19..29.
Deterministic enumeration selects the minimum of worst predicted input delay
plus worst predicted output delay. Original placement strengths are retained.
The hook refreshes architecture information, checks every original occupied
BEL and the clone for legality, checks every original port connection against
its expected preserved/selected-ENA value, and runs the context checker.
Failure aborts before routing.

Snapshots are written to `<prefix>.before.json` and `<prefix>.after.json`.
Ordinary JSON omits architecture pin state, so each snapshot also has a
`.pins.tsv` sidecar recording effective folded-polarity/constant states for
every cell port and retained architecture-only pin.
`LOCALITY_PROBE` log lines identify the original driver and BEL, cloned BEL,
selected users, legal candidate count and predicted delays. Independently
validate the snapshots before interpreting timing:

```sh
python3 mistral/tests/enable_locality_probe.py \
  /absolute/existing/directory/probe.before.json \
  /absolute/existing/directory/probe.after.json
```

The checker canonicalizes JSON net IDs, requires exactly one added LUT/net,
checks its truth table and input nets, requires all original effective pin
states unchanged (including selected FF ENA pins), checks every clone pin
against the original driver using the sidecars, verifies every original
cell and BEL is unchanged except selected ENA connections, and checks all
ports, original net metadata and design parameters. Its negative control
rejects the unchanged design because the required clone is absent.

Build, disabled-mode, snapshot and full-route results are recorded separately
in the FES issue-264 enable-locality investigation. The checker proves the
selected combinational duplication; it does not establish a timing gain. This experiment does not establish any fault in logical
fanout weighting; Quartus also uses a similarly shared enable on its faster
matched path.
