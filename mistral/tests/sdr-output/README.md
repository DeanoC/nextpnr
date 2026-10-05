# Dedicated SDR output register

This fixture exercises the explicit `FAST_OUTPUT_REGISTER ON` QSF assignment
for a directly connected `MISTRAL_FF` and `MISTRAL_OB`. nextpnr absorbs that
FF into the constrained GPIO as `MISTRAL_SDROUT`, preserving the existing
data and clock nets while selecting the dedicated Cyclone V output register.

The FF must directly drive the pad, with one Q consumer, a non-inverted clock
source, and inactive synchronous clear/load. Constant data is supported. Clock
enables and asynchronous clears are covered by the broader
[I/O register fixture](../io-registers/README.md). `FAST_OUTPUT_REGISTER OFF`
leaves the fabric FF in place. Inverted clocks, Q fanout, held-active clear and
unsupported register parameters are rejected instead of changing the design.

The bitstream settings are taken from a Quartus Prime Lite 17.0.2 compile of
the same one-register design on `5CSEBA6U23I7`, pad W15. Quartus reports one
I/O register and zero fabric registers. The decoded settings are
`OUTREG_OUTPUT_SEL=SEL_SDR`, `OEREG_HR_CLK_EN=1`,
`RBOE_LVL_FR_CLK_EN=1`. That original unconstrained fit selected both
output-register delay selectors at `0x1f`. The current backend keeps the
database's zero defaults, matching the externally constrained
[GPIO reference fits](../gpio-timing/README.md). Maximum D5 delay adds up to
1.180 ns to their clock-to-pin paths. Quartus chooses delay compensation per
design; nextpnr requires an explicit `D5_DELAY` assignment. The host test
decodes zero defaults and an explicit `D5_DELAY 31` override, as well as the
GPIO `DATAOUT.0`/`CLKOUT.0` routes. Changing this default changes the bitstream
and requires external setup/hold and hardware validation.

The qualified profile now times fabric data and enable setup/hold. External
clock-to-pad timing remains uncharacterized; a reported fabric Fmax does not
certify HDMI or another external interface. `set_output_delay` on this
registered pad is tested for explicit rejection of the missing boundary.

The four original nextpnr RBFs are retained under `host-artifacts/`, with their
uncompressed hashes and sizes in `host-results.txt`. Those historical files
precede the zero-delay default and do not describe current test output.

Run the host regression with:

```sh
python3 mistral/tests/sdr-output/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/sdr-output
```

This is host-only evidence. It does not modify the Pong RTL or claim a
measured pin waveform.
