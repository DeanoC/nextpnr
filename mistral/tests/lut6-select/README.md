# Physical LUT6 select timing

PR172 exchanged the physical E/F delay bounds for LUT6 cells. Independent
Quartus 17.0.2 fits on 5CSEBA6U23I7 show that F is the faster input in both
L5 and L6 modes. These fixtures validate the physical naming before selecting
existing delay bounds; they do not recalibrate absolute silicon delays or
establish timing acceptance for Pong/C64 or an SDRAM board.

The two asymmetric fixtures use `cyclonev_lcell_comb` with mask
`d2c4b6e1987a35f0`, registered inputs and outputs, and a 10 ns clock. The
bottom fixture constrains LUT6 to LABCELL_X29_Y77_N39. Virtual peripheral
pins keep the measured path internal. The fitted locations and resource
assignments in the QSF were obtained using routing back-annotation.

The audit traces each source register through decoded RBF routes to its
physical ALM input, then evaluates nextpnr's mask-generation convention for
all 64 configurations. Both halves exactly reproduce the Quartus mask.
Swapping E/F produces 20 mismatching bits in either fixture; swapping C/D
also fails. Physical E/F in this audit refer to decoded libmistral ports,
not source Verilog input order or an assumed TimeQuest pin suffix.

At Slow 1100mV 100C, the worst transition of the observed CELL arcs is:

| Half | Physical E | Physical F |
| --- | ---: | ---: |
| Top | 297 ps | 89 ps |
| Bottom | 335 ps | 90 ps |

Four-corner transition reports, decoded bitstreams, inputs and audit results
are retained here. The reports are calibration fixtures, not complete timing
reports for an OSS-routed design. Load, slew, speed grade, device and PVT
limits remain; the change restores the existing E/F bound selection rather
than fitting new constants from these samples.

Check the retained evidence without Quartus:

```sh
python3 mistral/tests/lut6-select/check.py mistral/tests/lut6-select/asymmetric
python3 mistral/tests/lut6-select/check.py mistral/tests/lut6-select/asymmetric-bottom
```

To reproduce a fit, copy its Verilog/QPF/QSF/SDC into a fresh directory and
run `quartus_sh --flow compile top`. Extract TimeQuest CELL edges at every
available corner, retaining launch register names, input suffixes and all
four transition delays. Run `quartus_cdb top --back_annotate=routing`,
convert the SOF with `quartus_cpf -c output_files/top.sof top.rbf`, then use
`mistral-cv decomp 5CSEBA6U23I7 top.rbf top.bt`. The audit requires that
decoded bitstream and back-annotated QSF, not a newly fitted placement alone.

An API probe against pinned libmistral 8fcc4cb4 also showed that nextpnr's
LUT6 requests TMODE=C_E and BMODE=D_E are rejected. The accepted choices
are TMODE=D_E and BMODE=C_E, which are the device defaults. Explicitly
writing those choices preserves the existing truth convention; checking the
return value prevents another silent failure. This issue predates PR172.
