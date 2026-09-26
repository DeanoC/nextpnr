# Registered M10K outputs

`MISTRAL_M10K` and `MISTRAL_M10K_TDP` accept two optional JSON parameters:

| Parameter | Meaning |
| --- | --- |
| `CFG_OUT_REG_A` | Select the M10K A-side output register. |
| `CFG_OUT_REG_B` | Select the M10K B-side output register. |

Both default to `0`, preserving the existing flow-through output settings.
On a true dual-port cell the parameters select the A1Q and B1Q paths
independently. A simple dual-port cell has only a logical B read output, so
`CFG_OUT_REG_B=1` selects its B register. A 40-bit simple-dual read is split
across both physical output halves; any output-register request selects both
halves so the logical word has one latency. `CFG_OUT_REG_A` is accepted for
that 40-bit physical layout and has the same whole-word effect. Narrow
simple-dual cells reject an A-side request. Asynchronous read mode rejects
both parameters because it has no clocked output.

The implementation writes the Cyclone V M10K `A_OUTPUT_SEL` and
`B_OUTPUT_SEL` fields. Existing output-clear handling may also select a
register when an ACLR input is active. Output clock selectors remain those
chosen by the existing single-clock, dual-clock, mixed-width and true-dual
port paths.

Quartus Prime Lite 17.0.2 reference designs for a registered 20-bit
dual-port output and a registered 40-bit simple-dual output are retained in
[`oracle/registered-output`](oracle/registered-output/README.md). They are
configuration evidence only; no hardware acceptance is claimed here.

Run the host regression with the paired M10K inference build:

```sh
python3 mistral/tests/m10k/output_register.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/pins.qsf --sdc /path/to/clocks.sdc \
  --output /tmp/m10k-output-register
```

The test checks 20-bit SDP B registration, the two-half 40-bit mapping,
independent TDP output selectors, rejected combinations, compressed RBF
generation, one-M10K utilisation and a 50 MHz timing constraint.
