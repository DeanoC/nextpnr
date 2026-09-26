# Dedicated DDR clock forwarding

A width-one `altddio_out` with complementary constant data inputs now packs
into its directly connected `MISTRAL_OB` output pin. The packed cell type is
`MISTRAL_DDROUT`, sharing the existing GPIO BEL and IO utilization bucket.
No additional Mistral tables or Yosys changes are needed: the existing Yosys
blackbox survives synthesis, and nextpnr integrates it with the constrained
output buffer.

Use `datain_h=1`, `datain_l=0` for normal polarity, or 0/1 for inverted
polarity. The output uses the dedicated DDR registers, not a fabric LUT
clock mux. Enable and OE must be high, all resets low, `oe_out` unused,
`power_up_high="OFF"` and `invert_output="OFF"`. Unregistered/unused OE and
OFF/unused extended OE-disable parameters are accepted. `dataout` must drive
exactly one unidirectional output buffer. Widths above one, equal or malformed
constant/data combinations, enabled resets, dynamic controls and unsupported
parameters are rejected. This fixture covers constant-data clock forwarding;
changing fabric `datain_h`/`datain_l` is covered by the separate
[fabric-data DDR output fixture](../ddr-output-data).

The packer reuses an existing global clock buffer or inserts one before PLL
packing. This includes a minimal design containing only the reference input
and forwarded-clock output, and a PLL whose only consumer is the DDR output.
A 74.25 MHz source uses the separately checked 50 MHz fractional-N PLL profile.
An inverted output has the opposite polarity; no arbitrary phase adjustment
is added.

## Mapping and timing boundary

For the retained W15 example, GPIO(89,8) pad1 maps to DQS16(89,8) lane9.
CLK drives GPIO `CLKOUT.0`; DATAOUT0/1 have opposite constant inversion
states. DQS16 selects `OUTREG_MODE_SEL=DDR`,
`OUTREG_OUTPUT_SEL=SEL_SDR_DELAY` and `RBOE_LVL_FR_CLK_EN=1`.
DDR output mode skips the ordinary GPIO DQS bypass settings. Normal output
voltage/drive and OE configuration is preserved. Pads lacking the relevant
clock/data/DQS mapping are rejected.

CLK is classified as a timing clock input. There is no characterized
clock-to-pad delay or DDR fabric-data setup/hold model in this patch. The
host timing checks cover the diagnostic's fabric reference/PLL domains;
they do not establish the forwarded pin's delay, duty cycle or external
interface timing. Pin-level validation needs a scope or physical loopback.
No output-pin hardware acceptance is claimed here.

## Reproduce

```sh
python3 mistral/tests/ddr-output/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/ddr-output
```

The host matrix covers normal/inverted 50 MHz, minimal 50 MHz, and 74.25 MHz
PLL sources with and without fabric registers. It checks the packed output,
clock route, constant polarity, complete per-lane DQS settings, RBF writing
and applicable fabric timing. Nine invalid requests must fail.

The [oracle](oracle) contains a portable Quartus17.0.2 project, its exact
gzipped RBF and a hash/settings manifest. Run `quartus_sh --flow compile top`
in a copy of that directory. Decompress `top.rbf.gz` and run `mistral-cv decomp
5CSEBA6U23I7 top.rbf top.bt` to inspect the reference. W15 is the DE10-Nano LED0
pin in these host fixtures; do not assume it is wired to an external device.

All five exact host RBFs and their hashes are retained in
[host-artifacts](host-artifacts). The normal and inverted 50 MHz diagnostics
report fabric Fmax 263.089 MHz; the PLL diagnostic reports 319.285 MHz against
the 50 MHz reference and 274.650 MHz against the 74.25 MHz source. Minimal cases
have no fabric register paths and therefore no Fmax entries. These figures
are not output-pin timing guarantees. No hardware was programmed.

The retained ordinary GPIO regression source and RBF used identical synth
JSON on the base and changed backends and produced byte-for-byte identical
RBFs, preserving the existing output bypass configuration. Reproduce it by
synthesizing `host-artifacts/ordinary.v` with the same flags as `check.py`,
then routing that JSON with each backend using `pins.qsf` and `clocks.sdc`.
