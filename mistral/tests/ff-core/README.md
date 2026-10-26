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

Clock-to-Q ends at the reported FF Q pin; subsequent output-buffer and route
delay stays outside it. Input requirements subtract the internal clock CELL
delay from the data/control CELL delay, retaining the report's intrinsic
adjustment. These primitive pin observations alone do not establish
equivalence to native virtual endpoints or timing-model bounds for every
configuration. The timing engine, default models and source locks are
unchanged. No native corner, reset pulse/MTBF, IO, board or hardware
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
