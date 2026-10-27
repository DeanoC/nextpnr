# Local FF timing references

The historical Mistral FF model uses one setup/hold pair and a clock-to-Q
value whose minimum equals its maximum. These fitted references expose the
different data, enable, synchronous-clear and clock-polarity cases needed
before replacing those constants.

Sixteen fixed-placement Quartus 17.0.2 Build 602 cases target `5CSEBA6U23I7`.
They cover LABs and MLABs, every ALM/FF slot, 100/50/74.25 MHz clocks, both
clock polarities, constant/positive/inverted enables, all three local
clock/enable selectors, and synchronous clear enabled and disabled. Four
cases also retain active-low asynchronous-clear deassertion observations.
The clocks used to expose IO paths are synthetic analysis constraints.
Synthetic IO and mixed-clock reset fixtures have fitted violations, retained in the records;
the fixtures are not board designs or reset-release qualification.

`reference.json.gz` contains all 101,120 local observations. `manifest.json`
binds their complete axes to the compiled source, fixture metadata, exact
fitted RBFs and raw-report hashes. Six compressed actual report witnesses
exercise the parser. Full raw captures are produced by the characterizer.
Failed captures remain in its output directory; it refuses an existing output.

Across the covered modes, clock-anchor-to-Q observations span 389–860 ps at
slow 100 C, 391–752 ps at slow -40 C, 183–282 ps at fast 100 C, and
158–222 ps at fast -40 C. These are observed pin ranges, not qualified
native timing envelopes.

The reader rejects missing/duplicate endpoints, a changed FF location or
clock polarity, a wrong data/control pin, nonfinite values, and an
asynchronous-clear assertion substituted for deassertion. Signed local
requirements and negative fitted fixture slacks remain signed.

`config.cc` reads the fitted RBFs without modifying them. It validates the
LAB/MLAB type, PKREG data selection, selected clock and inversion, enable
activation, and synchronous-clear configuration. Its mirrored PKREG and
TCLK/BCLK mappings follow `bitstream.cc`: FF0/FF3 use the top control group,
FF1/FF2 the bottom one; data and synchronous-clear selection follow the half.
The Python check binds every reported `d`/`asdata` identity to these bits.
For active E/F data and synchronous clear, the archive also retains the final
input-mux routing row before the FF pin. The configuration reader resolves
`TEF_SEL`/`BEF_SEL` and `SCLR_SEL` through Mistral's physical-port API, then
the Python check compares the selected input with every fitted report at
every corner and transition. Unused selectors do not describe a path and
are not claimed as validated selections. The input mux is the upstream
`BLOCK_INPUT_MUX` row; the later `LAB_INPUT` index already includes the
control selection and cannot independently verify it.

Clock-to-Q ends at the reported FF Q pin; subsequent output-buffer and route
delay stays outside it. Input requirements subtract the internal clock CELL
delay from the data/control CELL delay, retaining the report's intrinsic
adjustment. These primitive pin observations alone do not establish
equivalence to native virtual endpoints or timing-model bounds for every
configuration. This primitive-pin archive alone does not change a timing
model or qualify a native boundary. The timing engine, default models and source
locks remain unchanged. No native corner, reset pulse/MTBF, IO, board or hardware
acceptance follows from this reference.

Run the host regressions with the built configuration reader:

```sh
python3 mistral/tests/ff-core/test_reference.py \
  --config-reader build/mistral/nextpnr-mistral-ff-core-config
```

Rebuild every case into a new retained directory:

```sh
python3 mistral/tests/ff-core/characterize.py \
  --quartus-bin /absolute/quartus/bin \
  --config-reader /absolute/build/mistral/nextpnr-mistral-ff-core-config \
  --output /absolute/new-ff-reference
```

The characterizer runs only host compilers and file conversion. It does not
program a device, invoke a native router, select a GPU or contact a kit.

## Guarded native FF profile

`--mistral-ff-core-reference` selects a default-off local FF model for the
backend's native slow100 analysis. It requires `5CSEBA6U23I7`, the common-CLKA
graph, an exact nominal 50 percent clock with period at least 10 ns, driven
supported controls, and inactive synchronous-load/data overrides. Active
asynchronous clear also requires `--mistral-ff-reset-reference`; its existing
coverage and 170/270 ps limits remain separate.

`native-reference.json.gz` retains 326,912 observations from 52 fits. The
thirty-six added fits cover native combinations of constant/dynamic enables,
clear controls and local clock selectors that the original sixteen did not
exercise together. Mixed cases declare their active timing roles per FF:
inactive controls have no report endpoint. The manifest binds all source,
fixture, RBF and report identities. `model_reference.py` generates the compiled
table, including four corner datasets and explicit coverage for each role.
An unmatched mode or incomplete physical identity is fatal.

Clock-to-Q starts after the dedicated LAB clock-input buffer and ends before
the FF output buffer. Local LUT data starts at `d`; E/F data and controls
start after their input-mux buffers. Input constraints subtract the local
clock delay using both fitted clock/data anchors, including their IC
increments. The model rounds Q bounds outward by 1 ps and input constraints
by 3 ps, accounting for the five rounded report quantities. It preserves
signed requirements and distinct early/late Q values. Bounds pool the fitted
FF slots within each declared physical mode. SCLR DIN1 and DIN3 selections
have separate mode keys; E and F bypass data have separate coverage and bounds.
The current native graph requires the measured DIN3 SCLR mode.

Before configuration the placer uses conservative unions over possible
physical choices. After configuration the model checks the actual owner,
PKREG/E/F selection, physical clock/enable index and polarity, active controls
and net-owned routing cuts. The default allocator may use virtual CLK0 for
physical CLK/ENA1; the RBF index owns model selection. SCLR's direct physical
GOUT sink needs an explicit terminal-buffer observation; the profile charges
it once on the routed net. The ordinary model retains its existing boundary.

Regenerate the header or check it against the fitted artifacts:

```sh
python3 mistral/tests/ff-core/model_reference.py --check \
  --config-reader build/mistral/nextpnr-mistral-ff-core-config
```

Rebuild the complete native-cut reference into a new directory:

```sh
python3 mistral/tests/ff-core/characterize.py \
  --manifest native-manifest.json --native-cuts \
  --quartus-bin /absolute/quartus/bin \
  --config-reader /absolute/build/mistral/nextpnr-mistral-ff-core-config \
  --output /absolute/new-native-ff-reference
```

The public profile uses the slow100 table to match the native routing model.
The other corner data does not qualify native fast-corner routing, other
primitive/PLL/pad/HPS models or a physical board. No timing exception, packing
relaxation, clock change, source-lock update or hardware acceptance is implied.
