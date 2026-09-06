# First Cyclone V PLL profile

This is a bounded starting point for PLL support. The HPS diagnostic has
measured the expected output/reference frequency ratio and asserted lock on
hardware. It is not general-purpose PLL support or system-image acceptance.

The test uses the existing Yosys `altera_pll` blackbox, without a Yosys patch.
One physical FPLL is reserved per cell. On `5CSEBA6U23I7`, Mistral enumerates
six FPLL sites. The currently accepted configuration is:

- Reference: 50.0 MHz from dedicated board clock pin V11.
- Output: one 25.0 MHz clock, zero requested phase shift, 50% duty cycle.
- Direct mode with integer feedback, M=12, N=2, VCO=300 MHz, C6=12.
- `rst` tied low; optional `locked` status output.
- One existing MISTRAL clock buffer on the output; no other unbuffered sinks.

Parameters are deliberately restricted to the canonical values in `top.v`.
Unsupported device, pin, frequency, phase, duty cycle, clock count, reset,
reconfiguration ports, or output topology fail explicitly. Missing frequency
and mode parameters also fail. This profile has no runtime reset/relock support.

## Implementation

Mistral already provides the FPLL geometry, port connections and PRAM fields.
No Mistral table changes are needed. `create_plls()` imports the FPLL positions,
the dedicated CLKIN.0 input links, and C6 links to CMUXHG PLLIN ports.
The current clock-buffer implementation still exposes only CMUXHG subblock 2.

The selected path is GPIO.032.000.0 -> FPLL.000.014 -> C6 ->
CMUXHG.000.035 PLLIN.14 -> global clock subblock 2. `CLKIN_0_SRC=4`
selects the dedicated input and `INPUT_SEL.2=0x16` selects that PLL output.
The packer binds the connected PLL and clock buffer together. When Yosys
promotes a reference also used by fabric registers, the packer reconnects
the PLL to the dedicated pad tap and retains the reference's fabric buffer.
nextpnr-only
wires represent the dedicated clock taps; bit generation programs their PRAM
selectors separately from the ordinary CRAM routing graph. The GPIO fabric
DATAIN wire serves as the logical source for its dedicated COMBOUT tap.

The FPLL writer programs M and C high/low divisors to 6, with N=2 from the
Mistral default high/low values of 1. `FBCLK_MUX_2=1` selects direct feedback.
VCO and C6 enables, bandwidth, charge pump, lock filters and reset inversion
match the Quartus 17.0.2 reference. In addition, the unused auxiliary bandgap
at FPLL (0,73) must be powered down via `PL_AUX_BG_POWERDOWN=1`. Matching
only the 28 settings/inversions in the selected FPLL is insufficient: the
first hardware diagnostic reported no lock and zero output counts. Adding
that single field produced lock and the expected frequency ratio. This
setting is part of this specific device/reference profile, not a rule for
arbitrary PLL configurations. No analog parameter solver is implemented.

The packer constrains both sides of the output buffer to 25 MHz and checks
conflicting clock periods, including the 50 MHz input pin constraint. Downstream
synchronous paths receive the ordinary backend timing analysis. The dedicated
reference tap has no Mistral analog timing model: its arc uses the backend
estimate. PLL jitter, phase alignment to the input, lock-acquisition time,
and reset/relock behavior have not been characterized. Direct mode does not compensate
clock-network delay; see the [Altera PLL guide](https://cdrdv2-public.intel.com/666336/altera_pll-683359-666336.pdf).

## Reproduce

Run from the nextpnr checkout, using absolute executable paths:

```sh
python3 mistral/tests/pll/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /path/to/pll-results
```

The script synthesizes `top.v`, checks one PLL and the 25 MHz constraint,
routes and writes a compressed RBF, decodes the actual configuration, and
tests rejection of unsupported parameters, ports, reset, output fanout,
reference pin and conflicting input/output clocks. It programs no hardware.
Before the implementation, the same synthesis fixture fails placement with
`no BELs remaining to implement cell type 'altera_pll'`.

For an independent Quartus reference, run in an empty scratch directory:

```sh
quartus_sh -t /absolute/path/to/mistral/tests/pll/oracle.tcl
quartus_sh --flow compile top
mistral-cv decomp 5CSEBA6U23I7 output_files/top.rbf top.bt
```

Quartus 17's generic wrapper needs the `PLL_COMPENSATION_MODE DIRECT`
assignment as well as `.operation_mode("direct")`. Its IP generator emits
this in the generated QIP; without it, the fitter produces Normal mode and
an additional global feedback route. Check the fitter's PLL Usage Summary
for **Direct**, **none** for feedback clock type, and M=12/N=2/C=12.
Quartus chooses clock mux subblock 0; nextpnr uses its existing subblock 2.

## Source bases

- nextpnr fork: `99cf0efd724a07b0df3a8d91c29aaec6613bfa1f`, including DSP support
  on upstream `7d4f72c0aabc15da932748a54e82a6ff7b41921e`.
- Mistral fork: `328cfb8046d6bcb979fa69df7cfb95bd6f7e73f8`, including the DSP
  tile fix on upstream `bfa096c1deac6180a3eee784693c28dac491ab18`.
- Yosys: `13b43f8c85ec430a33ee55d058fb4c32b42b6910`.

No misteross ladder changes or lock updates are part of this test. Integration
needs selection of the resulting nextpnr revision after acceptance; it must
not authenticate a modified binary using an unchanged toolchain receipt.

## HPS frequency diagnostic

`diagnostic.v` uses the same PLL profile and one HPS GP block. `pll_meter.v`
divides the PLL clock by 256, synchronizes the resulting single bit into the
50 MHz reference domain, and counts rising edges over exactly 2^20 reference
cycles (nominally 20.97152 ms). Expected count: 2048 +/- 1. The result is held
unchanged until the next request, allowing coherent byte reads over HPS GP.

Build and inspect it using `diagnostic.py` with the same four executable/output
arguments as `check.py`. The report must show one PLL, one HPS GP, two clock
buffers, no DSP/RAM, and clocks `clk25`=25 MHz and `meter.refclk`=50 MHz.

`meter_sim.cpp` tests correct frequency, stopped clock, doubled frequency,
repeated requests, sampled lock loss and stable snapshots using a 2^12-cycle
window. To run from the repository root:

```sh
verilator --cc --exe --build --top-module pll_meter -GWINDOW_BITS=12 \
  --Mdir /absolute/output/meter-sim \
  "$PWD/mistral/tests/pll/pll_meter.v" "$PWD/mistral/tests/pll/meter_sim.cpp"
/absolute/output/meter-sim/Vpll_meter
```

HPS GP protocol:

| Register bits | Meaning |
| --- | --- |
| GPI [31:16] | Signature 0xD711 |
| GPI [15] | Measurement busy |
| GPI [14] | Completed request toggle; initial zero is not a completed measurement |
| GPI [13] | Synchronized PLL lock status |
| GPI [12] | Sampled lock loss during the measurement |
| GPI [7:0] | Selected result byte |
| GPO [1] | Request toggle |
| GPO [0] | Result byte select, zero=low/one=high |

Allow one outstanding request: read `done`, toggle the request to its opposite,
wait for `busy=0` and matching `done`, then read both bytes before requesting
again. Very brief lock pulses may escape the synchronizer; this is a frequency
and sampled-lock diagnostic, not a jitter or transient-lock characterization.

After loading the diagnostic through the designated kit's existing `kit.py`
lease session, run `probe.sh` on that target while retaining the lease. It
checks the signature before writing GPO, takes three snapshots, and requires
the expected count, asserted lock and no sampled lock loss. It does not claim,
program, stop or recover hardware itself. The register addresses are FPGA
manager GPI 0xFF706014 and GPO 0xFF706010. Stop and release through the kit
client afterwards. Use the current client with Stop/reboot recovery: it handles
`reboot_required` through the development-reboot endpoint and waits for a new
boot ID and free lease. This validation initially used the older `2d171c8`
client without that protocol and used manual recovery after HDMI/I2C cleanup
failed; that manual path is not the workflow for future tests.

Hardware controls used the identical diagnostic RTL: Quartus 17.0.2, a Mistral
raw load/save of its RBF, and Mistral decompile/recompile of its settings all
passed. They distinguish the auxiliary-bandgap omission from serialization
and other unmodeled configuration. The corrected nextpnr diagnostic emitted
SHA-256 `2d5be08a315dd7e5e9f620954e588e40340dbcfff7c735da707c3f68b8eb0bcb`.
