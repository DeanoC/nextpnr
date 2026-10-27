# LUT and carry primitive references

These Quartus Prime Lite 17.0.2 references cover the local combinational
primitives on `5CSEBA6U23I7`. They supply the missing four-corner early/late
observations needed before replacing the backend's fixed LUT/carry constants.
They do not change a compiler timing model or qualify a board.

Eight fixed fits cover LAB and MLAB logic. Each block has a paired L5 case,
separate top/bottom LUT6 cases, and a twenty-stage arithmetic carry chain.
Every ALM position and both halves are exercised: 120 fitted primitive
placements in total. Independent launch FFs are pinned in the adjacent LAB
to exclude the fitter's local register-feedback modes. Carry begins at
constant zero; every sum and the final carry are captured in preserved FFs.

The retained corpus contains 3,664 primitive arc records and 13,968 valid
transition intervals across all four operating corners. Each interval comes
directly from `get_edge_info -delay -min/-max` for its valid input/output
transition. Positive/negative unate arcs retain their absent transitions;
an absent observation is never filled with zero. Zero-delay observations
on some carry halves remain explicit.

`bindings.tsv` independently identifies the launch register of every fitted
data pin. Its physical FF location is checked against the back-annotated QSF,
then decoded RBF routes identify the actual ALM input. The checker evaluates
the source truth tables against each native LUT mask, including the MLAB
permutation and a carry operand exchange identified by the routed private
inputs. Local feedback and unsupported mode selections fail the check.
Boolean unateness is also checked against the source function.

The direct edge inventory is complete; it does not depend on selected worst
paths. Another 128 full timing reports independently check the selected
setup/hold path increments against those edge queries at every corner and
transition. Every measured primitive appears in each report. Source files,
RBFs, decoded routing, reports and producer inputs are hash-bound. The CTest
decoder checks every retained RBF through the linked Mistral library again.

For example, the sampled physical A rising-input envelope is 440--601 ps
at slow 100 C and 170--226 ps at fast 100 C. The existing fixed A interval is
592--605 ps. The slow-only interval therefore cannot qualify fast-corner
hold analysis. Carry and sum observations also differ between the two ALM
halves; spreading a full-ALM delay evenly does not preserve individual short
paths.

These are fitted reference observations, not general worst-case bounds for
every LUT mask, input slew, output load, packing arrangement, cross-LAB
carry bridge, speed grade or device. Native cell/route boundary integration,
complete corner coverage of other primitives, clocks and IO, and physical
board/hardware acceptance remain separate work. No timing exceptions or
frequency changes are introduced.

Run the offline regressions:

```sh
cmake --build build --target nextpnr-mistral-lut-carry-decode
ctest --test-dir build -R '^nextpnr-mistral-lut-carry-' --output-on-failure
```

Rebuild the complete corpus into a new directory with a stock Mistral decoder:

```sh
python3 mistral/tests/lut-carry/characterize.py \
  --quartus-bin /absolute/quartus/bin \
  --decoder /absolute/build/mistral/nextpnr-mistral-lut-carry-decode \
  --output /absolute/new-lut-carry-capture
```

Validate a new case directly with `reference.validate_case(directory, case)`.
The source generator, decoded modes, all arcs, routing witnesses and full-path
increments must agree; failed attempts remain in their original directories.
`retain.py` imports a complete verified capture into an empty fixture tree.
It refuses to replace an existing corpus.
