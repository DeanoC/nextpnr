# Dedicated SDR output register

This fixture exercises the explicit `FAST_OUTPUT_REGISTER ON` QSF assignment
for a directly connected `MISTRAL_FF` and `MISTRAL_OB`. nextpnr absorbs that
FF into the constrained GPIO as `MISTRAL_SDROUT`, preserving the existing
data and clock nets while selecting the dedicated Cyclone V output register.

The supported profile is intentionally small: the FF must have constant
`ENA=1`, inactive `ACLR=1`, `SCLR=0` and `SLOAD=0`, with one Q consumer and a
non-inverted clock source. `FAST_OUTPUT_REGISTER OFF` leaves the fabric FF in
place. Inverted clocks, Q fanout, dynamic controls, constants and unsupported
register parameters are rejected instead of changing the design silently.

The bitstream settings are taken from a Quartus Prime Lite 17.0.2 compile of
the same one-register design on `5CSEBA6U23I7`, pad W15. Quartus reports one
I/O register and zero fabric registers. The decoded settings are
`OUTREG_OUTPUT_SEL=SEL_SDR`, `OEREG_HR_CLK_EN=1`,
`RBOE_LVL_FR_CLK_EN=1`, and both documented output-register delay selectors at
`0x1f`. The host test decodes the nextpnr RBF and checks these settings and the
GPIO `DATAOUT.0`/`CLKOUT.0` routes.

There is no Cyclone V GPIO-register setup/hold or clock-to-pad model in the
current Mistral timing database. The packed input is therefore an unclocked
timing endpoint and nextpnr emits a warning; a reported fabric Fmax does not
certify HDMI or another external interface. `set_input_delay` and
`set_output_delay` remain unsupported SDC commands and are tested for
fail-closed behavior.

The four exact nextpnr RBFs are retained under `host-artifacts/`, with their
uncompressed hashes and sizes in `host-results.txt`.

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
