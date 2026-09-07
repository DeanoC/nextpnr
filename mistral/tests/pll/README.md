# Cyclone V PLL support

This backend supports the bounded integer, fractional and phase profiles
described below, including two independent PLLs on the board reference.
Hardware evidence applies only to the explicitly recorded diagnostics; other
profiles have host-only validation. This is not general-purpose PLL support
or system-image acceptance.

The test uses the existing Yosys `altera_pll` blackbox, without a Yosys patch.
One physical FPLL is reserved per cell. On `5CSEBA6U23I7`, Mistral enumerates
six FPLL sites. The currently accepted configuration is:

- Reference: 25, 50 or 100 MHz from the dedicated V11 input route.
  The DE10-Nano onboard oscillator supplies 50 MHz; other references require
  an appropriate external physical clock source.
- Output: one clock from 1 to 100 MHz, expressed as decimal MHz with
  at most one-Hz precision and an exact C divisor
  from the checked 300/320 MHz reported VCO configurations; zero phase.
  Integer duty percentages are supported when exactly representable by the
  selected C high/low counters (see below).
- Direct mode with integer feedback. Prefer M=12/N=2 (reported 300 MHz);
  otherwise M=32/N=5 (reported 320 MHz). Only C6 drives the output.
- Active-high fabric-driven `rst`, or `rst` tied low; optional `locked` status output.
- One existing MISTRAL clock buffer on the output; no other unbuffered sinks.

Output frequencies use strings such as `"20 MHz"`, `"20.0 MHz"` or `"12.5 MHz"`. The reference accepts the same whole-MHz string syntax, restricted to 25,
50 or 100 MHz; other parameters keep the values in `top.v`.
The supported whole-MHz subset is 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 25,
30, 32, 40, 50, 60, 64, 75, 80, and 100. This is a bounded selector over two
checked feedback/analog configurations, not an arbitrary M/N analog solver.
Out-of-range or inexact requests (for example 7 MHz or 12.3 MHz) fail.
Unsupported device, pin, frequency, phase, duty cycle, clock count,
reconfiguration ports, or output topology fail explicitly. Missing frequency
and mode parameters also fail. Reset must be tied low or driven; a permanently asserted or undriven reset fails.

## Implementation

Mistral already provides the FPLL geometry, port connections and PRAM fields.
No Mistral table changes are needed. `create_plls()` imports the FPLL positions,
the dedicated reference links, and C6/C7/C5/C8 links to CMUXHG PLLIN ports.
Outputs prefer subblocks 2/3/1/0 respectively. Each lane can select any
imported PLL output; two PLLs share the available lanes as described below.

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

For the 25 MHz baseline, the FPLL writer programs M and C high/low divisors
to 6, with N=2 from the
Mistral default high/low values of 1. `FBCLK_MUX_2=1` selects direct feedback.
VCO and C6 enables, bandwidth, charge pump, lock filters and reset inversion
match the Quartus 17.0.2 reference. In addition, the unused auxiliary bandgap
at FPLL (0,73) must be powered down via `PL_AUX_BG_POWERDOWN=1`. Matching
only the 28 settings/inversions in the selected FPLL is insufficient: the
first hardware diagnostic reported no lock and zero output counts. Adding
that single field produced lock and the expected frequency ratio. This
setting is part of this specific device/reference profile, not a rule for
arbitrary PLL configurations. No analog parameter solver is implemented.

The packer constrains both sides of the output buffer to the selected frequency and checks
conflicting clock periods, including the selected input pin constraint. Downstream
synchronous paths receive the ordinary backend timing analysis. The dedicated
reference tap has no Mistral analog timing model: its arc uses the backend
estimate. PLL jitter, phase alignment to the input, and lock-acquisition time
have not been characterized. Functional reset/relock checks are described below.
Direct mode does not compensate
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


## Fabric reset and relock

`rst` maps to Mistral's existing FPLL `NRESET0` fabric endpoint. Quartus 17's
active-high routed reset uses the default inverter bit (0), whereas the
unconnected folded-low profile needs bit1. No other FPLL PRAM fields or Mistral
tables change. The input is an asynchronous timing endpoint: synchronous Fmax
does not characterize reset pulse width, recovery/removal, or analog lock time.

`reset.py` builds `reset.v` with the same meter and verifies the actual routed
reset, inverter bit, resources and both clock constraints. `--invert` also
exercises a fabric inverter driving reset. Its HPS signature is
`0xD712`. GPO bit2 requests reset; two reference-clock registers drive the PLL,
and GPI bit11 echoes that reset. Other status and measurement bits match the
fixed diagnostic. The reference clock remains running throughout reset.

Run `reset_probe.sh` on the leased target after loading the reset RBF. It tests
ten assert/release cycles: reset echo1, lock0, count0 while held; then reset
echo0, lock1 and 2048 ±1 after release, without sampled lock loss during the
measurement. It is a functional diagnostic, not a lock-time specification.
The probe neither loads an RBF nor manages the kit lifecycle.

On 2026-09-06 the reset diagnostic RBF SHA-256
`5e48f9643c85a94bafeaaec2076c002710b15a8eb2b811dd42f6809da34880a4`
passed all ten hardware cycles (0 while reset, 2048 after relock). Its reported
Fmax was 214.684 MHz against the 50 MHz reference constraint and 331.126 MHz
against the 25 MHz output constraint. The fixed-profile RBF hash and DSP
regressions remained unchanged.


## Configurable frequency validation

`mistral/pll.h` selects the first exact C divisor from the two configurations.
The packer and bitstream writer use the same selection, without accepting user
parameters that override internal divider or analog settings. Clock periods
are represented at nextpnr's picosecond resolution, so rates such as 30 MHz
have a small report-rounding difference; tests allow only that quantization.

| Reported VCO MHz | M/N | BW / CP | M low / phase preset |
| --- | --- | --- | --- |
| 300 | 12/2 | 7 / 1 | 1 / 0 |
| 320 | 32/5 | 6 / 2 | 4 / 2 |

Quartus 17.0.2 oracles for 20/100 MHz confirm C=15/3 with odd-C 50% duty
correction. The 40/80 MHz oracles confirm the second configuration with C=8/4
and unchanged feedback presets. Odd N=5 does not enable N duty correction.
Mistral already provides all required fields; no table changes are needed.
The existing 25 MHz configuration remains the preferred first choice.

The rates above are **Quartus-reported rates after the VCO post-divider**, not
physical oscillator frequencies. The [Cyclone V PLL specifications](https://docs.altera.com/r/docs/683801/current/cyclone-v-device-datasheet/pll-specifications)
explain this distinction. Both configurations keep the checked `VCO_DIV=0`,
lock filters, auxiliary bandgap, reset polarity, and dedicated clock route.

Run the standalone selector regression:

```sh
c++ -std=c++17 -I mistral mistral/tests/pll/frequency_config.cpp -o /tmp/pll-config-test
/tmp/pll-config-test
```

`frequency.py` accepts the same tool/output arguments as `reset.py` plus
`--mhz N`. It generates a diagnostic source from `reset.v`, identifies it with
GPI signature `0xD713`, and checks the routed reset, both clocks, actual counter
fields and analog settings. `frequency_meter_sim.cpp` drives the production
meter at 20/40/100 MHz with `WINDOW_BITS=12`, including stops and restarts.

After loading the corresponding frequency RBF under a kit lease, run
`sh frequency_probe.sh 20` (or `40` / `100`) on the designated target. Each run
checks ten held-reset zero counts and ten recovered counts around
`frequency_MHz * 4096 / 50`, allowing one edge at the window endpoints.
This establishes frequency ratio and functional reset/relock, not analog
jitter, phase accuracy, duty-cycle tolerance, or a specified lock time.


On 2026-09-06 the 20, 40 and 100 MHz OSS artifacts each passed ten hardware
reset/relock cycles. Held reset always measured zero; released outputs measured
1638–1639, 3276–3277 and 8192 respectively, with lock asserted and no sampled
lock loss during measurement. Reported Fmax was 216.732 MHz for the 50 MHz
reference domain and 326.584 MHz for the output domain in all three designs.
Each compressed RBF is 1,955,948 bytes.

| Output MHz | Hardware-tested RBF SHA-256 |
| --- | --- |
| 20 | `771f5eb504896c411e3e61fcea64c465d90a4149c362a7b605c32b14d87c1d42` |
| 40 | `0f63eefc0f8424f46af1e0c3776aff2a72af0b8bb2b653a798585fd2710700af` |
| 100 | `009bc5914818c400455c061e90725e9fd7f0af80404fcdd8f08e892ee9e52b47` |

## Two simultaneous outputs

The dual-output profile accepts `number_of_clocks=2` and exact decimal output
frequencies from 1 to 100 MHz. Both must divide exactly from one shared checked
feedback configuration, tried in order: reported 300, 320, then 400 MHz.
For example, 40/25, 20/100 and 40/64 MHz are supported; 25/32 MHz is rejected
because no checked configuration divides exactly into both. Equal output
frequencies are supported. Both require zero phase and 50% duty; the existing
V11 reference route, direct mode and reset rules still apply. Single-output
selection remains restricted to its original 300/320 MHz tuples.

The original 25/40 MHz pair retains the Quartus 17.0.2-checked 400 MHz configuration:
M16/N2, BWCTRL7, CP_CURRENT1, M presets1/0. C6 divides by16 and C7 by10.
Mistral already contains the connections and configuration fields; its pin
is unchanged. The first output retains CMUXHG(0,35) subblock2, PLLIN14.
The second uses subblock3, PLLIN13, with INPUT_SEL3=0x15 and PRE_SYNENB.
Subblock3 has only a dedicated PLL input in nextpnr; fabric clocks cannot
use it. The packer reserves both buffers with the PLL and assigns separate
output constraints. Odd C6 and C7 dividers enable duty-cycle correction.
No phase relationship timing model is added.

Run the host regression with the same tool paths used above:

```sh
python3 mistral/tests/pll/dual.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /absolute/path/dual-output
```

`dual.v` instantiates two independent reference-window meters. Signature
D714 identifies this diagnostic. GPO3 selects the meter, including its result
and status; GPO2 drives common reset, GPO1 requests measurement, and GPO0
selects the result byte. With a kit lease held, run `dual_probe.sh 25` and
`dual_probe.sh 40` on the target. These probes do not program or stop hardware.

On 2026-09-06 the compressed dual-output RBF had SHA-256
`b32e3972c4732fd89c22093c6a4724f0abd3113501a93c80767b24e81b2ca245`.
It used one PLL, three clock buffers, one HPS GP, and no DSP or RAM. Reported
reference/25 MHz/40 MHz Fmax values were 181.324/329.598/361.533 MHz.
On the designated kit, each output passed ten reset/relock cycles: both
returned zero while reset, then 2048 for 25 MHz and 3276–3277 for 40 MHz.
`kit.py stop` completed development reboot recovery and left the kit free.
This is exact-artifact functional diagnostic evidence, not characterization
of jitter, phase alignment or lock time, or native-image acceptance.

The fixed single-output LED regression retains its original RBF hash.
Additional BELs can change placement and artifact hashes for other designs;
previous hardware acceptance does not automatically transfer to rebuilt artifacts.

For other pairs, add `--mhz0 40 --mhz1 64` to `dual.py`. The runner checks
emitted dividers, analog settings, odd-duty bits and both clock constraints.
The selector test exhaustively checks all integer pairs through the accepted
range plus its immediate boundaries, including deterministic shared-tuple
selection. Routed regression pairs cover 25/40, 40/25, 20/100, 40/64, 80/80
and 1/1 MHz. These generalized-pair checks are host-only; the hardware record
above applies to the exact original 25/40 artifact. The existing hardware
probe only accepts that original pair and must not be used for arbitrary pairs.

## Checked reference frequencies

Reference selection uses a complete table of Quartus 17.0.2-checked tuples.
It does not scale M/N while assuming unchanged analog settings. Single-output
selection uses reported 300/320 MHz configurations; dual-output selection may
also use 400 MHz. Rows below give M/N, bandwidth, charge pump and M low/phase
presets. Dividers use `reference * M = output * N * C` for both outputs.

| Reference MHz | Reported VCO MHz | M/N | BW | CP | M presets |
| --- | --- | --- | --- | --- | --- |
| 25 | 300 | 24/2 | 6 | 1 | 1/0 |
| 25 | 320 | 64/5 | 3 | 2 | 7/3 |
| 25 | 400 | 32/2 | 6 | 1 | 1/0 |
| 50 | 300 | 12/2 | 7 | 1 | 1/0 |
| 50 | 320 | 32/5 | 6 | 2 | 4/2 |
| 50 | 400 | 16/2 | 7 | 1 | 1/0 |
| 100 | 300 | 6/2 | 8 | 1 | 1/0 |
| 100 | 320 | 32/10 | 6 | 1 | 1/0 |
| 100 | 400 | 8/2 | 7 | 1 | 1/0 |

The packer checks the input pad, dedicated reference net and buffered fabric
reference against the selected period, rejecting conflicting SDC constraints.
Both output periods remain independent. Frequencies outside the checked
reference set are rejected. No new input pins or PLL modes are enabled.

`reference_config.cpp` exhaustively checks all single and dual integer outputs
and all three reference rates, including analog table values and invalid
references. The existing frequency-selector tests continue to check the 50 MHz
baseline. `dual.py --reference-mhz 25 --mhz0 40 --mhz1 64` exercises a new
reference with the same host tool arguments as above. It supplies matching SDC
and checks emitted counters, analog settings and all three timing constraints.

Six Quartus oracle builds cover references 25/100 MHz with output pairs
25/50, 40/64 and 25/40 MHz, selecting all six new tuples. These use `dual.v`
with the corresponding reference/output parameters and matching input SDC,
plus `derive_pll_clocks` in the existing Quartus oracle flow. Generalized
reference validation is host-only. No new hardware acceptance is claimed;
the existing kit probes assume a physical 50 MHz reference and must not be
used unchanged with a different source.

## Decimal-MHz outputs with integer dividers

Output requests are parsed into integer hertz without floating-point rounding.
Up to six meaningful decimal places in MHz are accepted; trailing zeros do not
add precision. `12.5 MHz` and `12.500000000 MHz` are equivalent, while
`12.5000001 MHz` is rejected. Values remain within 1–100 MHz and require an
exact divider solution. There is no approximate-frequency fallback.

Single-output examples include 12.5 MHz (300 MHz /24) and 6.4 MHz
(320 MHz /50). Dual-output examples include 12.5/25, 6.4/64 and 12.5/40 MHz.
Both outputs still share one checked feedback configuration. The reference
remains restricted to the checked 25/50/100 MHz set. This does not enable
fractional-N operation or add analog settings, clock pins or PLL modes.

The bitstream selector uses integer arithmetic. Timing constraints use the
backend's existing picosecond resolution, so their reported decimal frequency
may differ slightly from the exact programmed frequency. `decimal_config.cpp`
checks parsing, exact divisibility, rejection and whole-MHz compatibility.
`dual.py --mhz0 12.5 --mhz1 40` validates fractional outputs with an independent
rational-arithmetic oracle. New decimal-output validation is host-only;
historical kit evidence remains specific to its recorded artifacts.

## Checked fractional-N profile

Set `fractional_vco_multiplier="true"` to request the separate, bounded
50 MHz reference to a checked 11.2896 or 12.288 MHz single-output profile. Other fractional-N
combinations are rejected except the checked dual-output pair below. The existing
V11 route, direct mode, zero phase, 50% duty and reset rules still apply.
The default `"false"` mode retains exact integer-divider behavior.

Quartus 17.0.2 selects M8, N1 (bypass), C6=33 and fractional word
K=472790000 (`0x1c2e33f0`) at 32-bit precision. The writer enables
DSM_OUT_SEL1, sets N high/low counts0, M high/low4, C6 high17/low16 with
odd-duty correction, BW7 and CP2. M presets1/0, lock filters0x19/2 and the
auxiliary bandgap powerdown remain as checked in the oracle. Mistral already
exposes all these fields; no table changes are needed.

Using `50e6 * (8 + K/2^32) /33`, the calculated output is
12,288,000.000019869 Hz, approximately +0.000001617 ppm from the request.
The packer reports requested/achieved/error and constrains the calculated
frequency at backend timing resolution. This is a specific checked approximation,
not a generic permitted-error solver. It does not silently relax the exactness
of integer mode. For comparison, Quartus with fractional mode disabled chooses
12,288,135.593 Hz for the same request, about +11.035 ppm.

`fractional.py` builds the diagnostic fixture and checks routing, timing,
emitted settings and unsupported requests. Its optional oracle comparison
checks all emitted FPLL settings. `fractional_config.cpp` checks the bounded
selector and achieved-frequency calculation. `fractional_probe.sh` runs under
a kit lease and checks signature D715, output counts and ten reset/relock cycles.
The reference-window meter expects 1006–1007 counts when running, with endpoint
tolerance; it cannot resolve the tiny calculated quantization error or measure
jitter. Programming and lifecycle remain the responsibility of `kit.py`.

On 2026-09-06 the compressed diagnostic RBF had SHA-256
`55b099c8f0230e7938c6444d11a767b675ef5b93d89e35194c1612a752bea891`.
It used one PLL, two clock buffers and one HPS GP, with no DSP/RAM.
Reference/output Fmax values were 195.274/340.716 MHz. The output constraint
reports 12.28803158 MHz due to picosecond timing quantization; this does not
change the programmed fractional word or its calculated frequency.
The exact artifact passed ten kit reset/relock cycles, returning zero while
reset and 1006–1007 counts after relock, with lock asserted and no sampled
lock loss. This is functional diagnostic acceptance, not measured frequency
precision, jitter characterization or native-image acceptance.

### 44.1 kHz audio-clock profile

The second checked fractional-N output is 11.2896 MHz (256 times 44.1 kHz).
Quartus 17.0.2 uses the same M8, N bypass and analog settings as the 12.288 MHz
profile, with C6=36 and K551954751 (`0x20e6293f`). Both C high/low counts are 18;
odd-duty correction is disabled. Calculated output is 11,289,599.972143251 Hz,
about −0.00246747 ppm from the request. This preserves the observed oracle word;
it does not claim that word is the closest possible numerical approximation.

Run `fractional.py --mhz 11.2896` with the normal tool arguments. Its full
FPLL settings assertion covers the new fractional word and even C divider.
The 12.288 MHz default remains unchanged. The diagnostic uses signature D716;
`fractional_probe.sh 11.2896` expects 924–925 running counts, with endpoint
tolerance, and zero under reset. It must run under the normal kit lease.

On 2026-09-06 the 11.2896 MHz diagnostic RBF had SHA-256
`0d95fe235f42e2adc5e8d66771c136b1a608ce325e907da7ad945ae0535bfc02`.
It used one PLL, two clock buffers and one HPS GP, with no DSP/RAM.
Reference/output Fmax values were 195.274/340.716 MHz, meeting 50/11.2896 MHz.
The exact artifact passed ten hardware reset/relock cycles with zero counts
while reset and 925 counts after each relock. This establishes functional
operation and sampled lock; it does not measure the calculated sub-ppm error
or jitter. The integer and 12.288 MHz baseline artifact hashes are unchanged.

Current `kit.py stop` completed development reboot recovery and left the kit free.

### Dual fractional-N audio clocks

With `fractional_vco_multiplier="true"` and `number_of_clocks=2`, the checked
pair is output0=12.288 MHz and output1=24.576 MHz, from the 50 MHz reference.
Swapped outputs and other pairs remain unsupported. Both outputs share M8,
N1 bypass and K=`0x5b18548b`, with C6=34 and C7=17. This is a separately
checked Quartus 17.0.2 configuration: the single 12.288 MHz fractional word
cannot be reused. C6 has even counts 17/17; C7 uses 9/8 and odd-duty correction.
Analog settings remain BW7/CP2 and the existing presets/filters.

Calculated outputs are 12,288,000.000134 Hz and 24,576,000.000268 Hz, both
about +0.000010905 ppm from their requests. Both generated clock constraints
use calculated frequency at backend timing resolution, and both errors are
reported. Independent per-output divider selection is not permitted.

`fractional_dual.py` checks the complete oracle FPLL configuration, three clock
constraints, reset routing and unsupported combinations. Its D717 fixture
has independent meters; GPO3 selects each meter's result and status.
Run `fractional_dual_probe.sh 12.288` and `fractional_dual_probe.sh 24.576`
under a kit lease. Expected running counts are 1006–1007 and 2013–2014,
respectively, with endpoint tolerance. These checks establish functional
operation, not precision or jitter characterization.

On 2026-09-06 the dual fractional diagnostic RBF had SHA-256
`1967c21bfb9778f2a0105ab5a7cbb2168b772acb5c8a3a2ec6631eb5d48190aa`.
It used one PLL, three clock buffers and one HPS GP, without DSP/RAM.
Reference/output Fmax values were 188.359/225.276/371.471 MHz against
50/12.288/24.576 MHz constraints. Each output passed ten hardware reset/relock
cycles, returning zero under reset and 1006–1007 / 2013–2014 counts after
relock with lock asserted and no sampled loss. Single fractional and integer
dual baseline RBF hashes remain unchanged. This is exact-artifact functional
diagnostic evidence, not a measurement of calculated ppm error or jitter.


## Integer duty cycle

`duty_cycle0` and `duty_cycle1` accept integer percentages from 1 through 99
when the selected C divisor represents that percentage exactly. For non-50%
outputs, high = C × duty / 100 must be integral, and high and low must each
fit the 1–255 counter range. The selector considers frequency and duty together
for both outputs. For example, 50/25 MHz at 25% duty uses the checked 400 MHz
configuration with C8/C16; the frequency-compatible 300 MHz configuration
cannot represent those duties. Existing 50% odd-divider correction is retained.
Fractional-N profiles still require 50% duty; phase shifts remain unsupported.

Quartus 17.0.2 references for 25 MHz at 25% and 75% use M12/N2/C12,
with C high/low counts 3/9 and 9/3 respectively. The host regression compares
all non-default selected-FPLL settings against those references. No Mistral
geometry or analog configuration changes are required.

The packer propagates high/low clock constraints and rejects contradictory
waveforms even when their periods match. A Yosys-generated inverter and second
clock buffer feeding only PLL-clocked FF clock pins are folded into the FFs'
hardware clock inversion. Timing then retains one clock domain with both edges.
The shared timing engine uses the high or low interval for opposite-edge slack
and Fmax, including clock skew. Critical-path JSON now includes `max_delay`
in nanoseconds, exposing the allowed interval for regression checks.

Run both pulse-width regressions with `duty.py --duty 25` and `--duty 75`,
using the same required tool/output arguments as `check.py`. Each checks both
edge directions (10/30 ns budgets), duty-scaled Fmax, compressed RBF output,
and invalid parameter/waveform rejection. `dual.py --mhz0 50 --mhz1 25
--duty0 25 --duty1 25 --skip-negative` exercises joint divisor selection.
`duty_config.cpp` supplies standalone selector boundary checks:

```sh
g++ -std=c++17 -Wall -Wextra -pedantic -I mistral \
  mistral/tests/pll/duty_config.cpp -o /tmp/pll-duty-config
/tmp/pll-duty-config
```

These duty-cycle checks are host-only; no pulse-width hardware acceptance is
claimed from the earlier frequency measurements.


## Static phase outputs

The original checked phase profiles use a 50 MHz reference, two integer 25 MHz outputs,
50% duty, `phase_shift0("0 ps")` and `phase_shift1` of `10000 ps`, `20000 ps`,
or `30000 ps`. Output 1 lags output 0 by 90°, 180° or 270° respectively.
The additional reference/rate profiles below expand this initial set. Other
shifts, mixed frequency pairs, fractional feedback and non-50% duty combinations
are rejected. Dynamic phase adjustment is unsupported.

Quartus 17.0.2 selects M12/N2 and C12 for both outputs. C7 `CNT_PRESET`
changes from its default 1 to 4, 7 or 10 respectively. All other selected FPLL
settings match the zero-phase pair. No Mistral geometry or analog changes are needed.

The packer declares a common phase origin for these two equal-period clocks.
The timing engine checks paths between them with the next capture edge:

| Output1 phase | Forward rise→rise | Reverse rise→rise | Forward rise→fall | Reverse rise→fall |
|---|---|---|---|---|
| 90° | 10 ns | 30 ns | 30 ns | 10 ns |
| 180° | 20 ns | 20 ns | 40 ns | 40 ns |
| 270° | 30 ns | 10 ns | 10 ns | 30 ns |

Coincident edges at 180° use the next strictly later capture edge for setup.
Physical clock skew remains part of the path delay. Hold checks use the previous capture edge.
The related paths contribute to the launch clock's reported Fmax and to
placement/router slack. No phase relation to the input reference is assumed.

Use the PLL-derived constraints for the shifted output: a separate SDC
`create_clock` cannot express this relationship and is rejected there.
`phase.py` uses the same tool/output arguments as the other runners and checks
both directions with rising/falling capture edges, configuration settings,
timing and invalid profiles. Select `--degrees 90`, `--degrees 180` or
`--degrees 270` (default 90). The latter two automatically verify and decompile
the [bundled Quartus references](fixtures/phase/README.md), requiring no Quartus
installation. Reproduce the Quartus reference using `phase-oracle.tcl` in an
empty directory, optionally passing `180` or `270` after the Tcl path, and then
the `quartus_sh --flow compile top`/Mistral decompile commands above. An optional
`--oracle-bt` compares the saved reference against the
embedded expected settings as well as checking every generated RBF.

The device-independent analyser test covers 32 phase/edge/skew combinations
and 64 hold-boundary checks. Run it without a device database:

```sh
git submodule update --init --recursive
cmake -S . -B /tmp/nextpnr-phase-tests -DARCH=generic \
  -DBUILD_TESTS=ON -DBUILD_PYTHON=OFF
cmake --build /tmp/nextpnr-phase-tests -j4
ctest --test-dir /tmp/nextpnr-phase-tests --output-on-failure
```

Validation is host-only. PLL jitter and physical phase accuracy have not been
measured, and a frequency meter does not establish phase accuracy.

See [the fork branch policy](../../../docs/mistral-fork.md) for integration and
upstream PR bases.

## Folded clock inversion: required Mistral correction

The ladder's [misteross issue #9](https://github.com/DeanoC/misteross/issues/9)
found that a folded falling-edge FF stayed at zero on hardware even though
host timing passed. Mistral's LAB/MLAB tables had interchanged the physical
`CLKx_INV` and `CLKx_SEL` addresses. Emitting inversion selected an unrouted
CLKB input; the FF therefore received no toggling clock.

Use Mistral `b28e30a` or a revision containing that correction. The
`MISTRAL_CORRECT_LAB_CLOCK_MUXES` capability guards FF and MLAB write-clock
inversion: an older library now produces an explicit error before RBF output.
The pin correction is required in addition to nextpnr's packing/timing changes.
Downstream toolchain locks must select the reviewed pair separately.

`duty.py` and `phase.py` now check the selected FF clock mux, inversion,
ungated enable and physical CLKIN.0 route using the corrected Mistral decoder.
These checks reject the original bad RBF. Mistral's separate fixed-placement
Quartus oracle tests verify the physical table addresses by whole-RBF equality
for all three clock channels in LAB and MLAB, avoiding a compiler/decompiler
pair that agrees on incorrect field labels.

On the designated kit, the original minimal 25% fixture remained at
`0xD7180002` while GPO[0] alternated. Changing only the misidentified clock
bits produced `0xD7180002`/`0xD7180003` as capture followed the input. This
isolates the clock-field defect; functional capture is separate from analog
pulse-width or phase-accuracy acceptance.

The freshly rebuilt tool pair produced the same corrected bytes and passed
20 input changes with lock asserted on 2026-09-06. Artifact:
`/home/deano/fes/out/dev/pll-invert/duty-25/rise-fall/top.rbf`, SHA256
`7f8ff8b607a1a28ebee4301360e2d23ef937834d3f904f7ef83fbd5838f69b79`.
The probe checked signature `0xD718`, lock at GPI[1] and capture at GPI[0]
after alternating GPO[0]. Evidence: `out/dev/pll-invert/final-probe.log` in
the FES workspace. The 25%/75% and phase host suites pass with the corrected
library; the existing rising-edge single, dual and fractional RBF hashes
remain unchanged.


## Three outputs from one PLL

`triple.v` checks the bounded integer 25/50/100 MHz profile on `5CSEBA6U23I7`,
with a 50 MHz reference on V11, direct mode, zero phase and 50% duty on all
three outputs. Additional checked references, frequencies, duties and phase
profiles are covered below; fractional feedback remains unsupported for three outputs. Existing reset/lock and buffered-output
rules apply; the four-output profile below is separately checked.

The profile uses M12/N2 and the checked 300 MHz configuration. Output0 uses
C6=12, output1 C7=6 and output2 C5=3, including the odd-divider duty correction
on C5. Mistral already describes the dedicated C5 connection to
`CMUXHG.000.035:PLLIN.15`. The new third-output buffer reserves horizontal
subblock 1 with input selection 23; C6/C7 retain subblocks 2/3. Subblock 1
has no fabric-input pip and only accepts the third PLL output. All three
buffers are checked for availability and bound together with the PLL.
No Mistral table changes are required.

The three generated clocks receive 25, 50 and 100 MHz timing constraints.
This extension does not add timing relationships between unequal-frequency
outputs; designs must handle crossings appropriately. It does not establish
phase alignment to the input reference or analog hardware acceptance.

Run with the same tool arguments as the other runners:

```sh
python3 mistral/tests/pll/triple.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-triple
```

The runner checks utilization, all three Fmax constraints, dedicated clock
buffer placements and mux settings, and invalid parameters/topologies/SDC.
It verifies hashes and decompiles the [bundled Quartus reference](fixtures/triple/README.md)
to compare every emitted FPLL setting, including auxiliary powerdown.
Quartus is needed only to regenerate that reference. Its clock-buffer lane
choices differ from the OSS placement; the documented Mistral connectivity
and explicit mux checks validate the OSS lanes.

Adding clock BELs can change placement and RBF hashes for existing profiles;
functional configuration and timing regressions remain the acceptance checks.
This work is host-only and does not update downstream toolchain pins.


## Four outputs from one PLL

`quad.v` extends the fixed triple profile with a 75 MHz fourth output:
25/50/100/75 MHz, 50 MHz V11 reference, direct integer mode, zero phase and
50% duty on all outputs, on `5CSEBA6U23I7`. Compatible frequency combinations
are covered below; five or more outputs are rejected. Reset/lock and output-buffer rules
remain the same.

M12/N2 and all analog settings remain unchanged. The fourth output uses C8=4
(high2/low2), whose existing Mistral link reaches
`CMUXHG.000.035:PLLIN.12`. A new reserved buffer uses horizontal subblock0
with input selection20; it accepts only the fourth PLL output and has no
fabric input pip. Existing buffer BEL indices are preserved by adding it
last. The packer checks availability of all four buffers before binding.
No Mistral table changes are required.

```sh
python3 mistral/tests/pll/quad.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-quad
```

The runner verifies all four clock constraints/Fmax, buffer placements and
CMUX selectors, then compares every emitted FPLL setting against the
[bundled Quartus 17.0.2 reference](fixtures/quad/README.md). The reference
contains the compressed RBF, hashes and portable regeneration inputs.
Quartus chooses different buffer lanes; the fixture documents the C8
horizontal input selection and the runner explicitly checks the OSS lane.
Unsupported parameters, disconnected/unbuffered fourth output and
contradictory fourth-clock SDC are rejected by the regression.

Validation is host-only. Unequal-frequency cross-clock timing relationships,
input-reference phase alignment and analog hardware acceptance are outside
this profile. Downstream locks and parent pins are unchanged.


## Compatible three- and four-output frequencies

The multi-output selector accepts exact decimal frequencies from 1 to 100 MHz
when every output has an exact, representable divider from one checked
300/320/400 MHz configuration. Zero-phase requests accept 25, 50 or 100 MHz
references on V11 with integer feedback. Representable duties are
covered below. This adds no new
analog tuples and no Mistral geometry or clock-buffer routes.

All outputs participate in selection, in the existing preference order
300, 320, 400 MHz. For example, 25/50 MHz alone prefers 300 MHz, but adding
80 MHz requires 400 MHz; 25/50/100/80 similarly selects 400 MHz because of
the fourth output. 75/80 MHz has no common checked tuple and is rejected,
even though each frequency is individually supported. Packing and bitstream
generation call the same selector and update every counter together.

```sh
python3 mistral/tests/pll/multi.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-multi
```

The runner checks 12.5/25/50, 25/50/80, 40/80/16/32 and 25/50/100/80 MHz
against bundled Quartus references under `fixtures/multi/`, including all
emitted FPLL settings, output constraints and dedicated buffer selections.
The decimal case exercises a non-whole-MHz frequency. Other cases exercise
the third or fourth output forcing a different common configuration.
Every output also gets nondivisor, inexact and out-of-range rejection checks.
The original `triple.py` and `quad.py` fixtures remain regression baselines;
optional `--frequencies` and `--oracle-fixture` arguments select another
checked reference.

Validation is host-only. The existing limitations on unequal-frequency
cross-clock timing and analog acceptance still apply; downstream pin updates
remain separate integration work.


## Independent duties on three and four outputs

Multi-output PLLs accept integer duty percentages from 1 to 99 when all
frequencies and high/low counts fit one checked 300/320/400 MHz configuration.
The selector considers the duty of every output before choosing a tuple.
50% duty keeps the existing odd-divider correction; other duties require
exact integer high/low counts, each within the existing counter limits.
Zero-phase profiles accept the checked 25/50/100 MHz references and require integer feedback.

For example, a 25/50/100 MHz triple with duties 25/50/25 needs 400 MHz:
25 MHz at 25% is representable at 300 MHz, but 100 MHz at 25% is not.
The four-output 25/50/50/100 profile with duties 25/50/50/25 similarly lets
the fourth output force 400 MHz. A 320 MHz mixed-duty reference checks
40/80/16/20 MHz with duties 25/75/25/75.

```sh
python3 mistral/tests/pll/multi_duty.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-multi-duty
```

All three [reference bundles](fixtures/multi-duty/) contain portable Quartus 17.0.2
inputs, compressed RBFs and checksums. The runner compares all FPLL settings
and global clock selectors, rejects out-of-range and unrepresentable duties
on every output, and routes independent rising-to-falling and falling-to-rising
paths for every clock. Their setup windows and reported Fmax must follow
the requested high/low fractions. `triple.py` and `quad.py` also accept
`--duties` alongside their frequency and oracle-fixture options.

No analog tuple, clock route or Mistral table is added. Tests are host-only;
physical pulse-width accuracy and hardware acceptance remain unmeasured.
Downstream pin changes remain separate integration work.


## Four-output quadrature profile

Four 25 MHz outputs can use the static phase sequence 0/90/180/270 degrees
with a 50 MHz V11 reference, integer feedback, direct mode and 50% duty.
Set `number_of_clocks=4`, all output frequencies to `25 MHz`, and
`phase_shift0/1/2/3` to `0 ps`, `10000 ps`, `20000 ps`, `30000 ps`.
This is an example of the selectable quarter-cycle phases described below.
Other frequencies/duties and fractional feedback remain rejected whenever
a phase shift is requested.

C6/C7/C5/C8 all divide by 12. The existing C7 preset 4 is joined by C5 preset 7
and C8 preset 10, confirmed by the [bundled Quartus reference](fixtures/quadrature/README.md).
No Mistral table, analog configuration or clock route is added.

All four outputs and their buffers share a PLL phase origin. The timing
engine uses the next strictly later capture edge, modulo the 40 ns period,
including wraparound and coincident edges. No phase relation to the input
reference is assumed. Explicit SDC clocks on shifted outputs or folded
inverted buffer outputs are rejected because `create_clock` cannot express
this relationship in the current frontend.

```sh
python3 mistral/tests/pll/quadrature.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-quadrature
```

The runner verifies reference hashes and all emitted FPLL settings, four
clock-buffer placements and selectors, phase crossing setup budgets and
reported Fmax, plus invalid profiles and contradictory SDC constraints.
A combined design checks worst-path reporting; six compact four-path designs
expose all 24 ordered-pair/capture-edge combinations individually because the
report retains only the worst destination per source clock.
The reference includes portable regeneration inputs and the fitter's phase
table, so reviewers do not need private artifacts or Quartus to run checks.

Validation is host-only. Physical phase accuracy and analog clock behavior
have not been measured; downstream pins and hardware integration remain
separate work.


## Selectable quarter-cycle phases

Three or four 25 MHz outputs can independently select 0°,90°,180° or270°,
with output 0 fixed at 0°. Use the exact phase strings `0 ps`, `10000 ps`,
`20000 ps` or `30000 ps` for the corresponding `phase_shift` parameters.
Integer feedback, direct mode and 50% duty are required whenever any output
is shifted. The reference and output-rate extensions below expand the original
50 MHz reference / 25 MHz output profile. Other angles, nonzero output 0 and unsupported
frequency/duty combinations are rejected. All-zero phase profiles retain
the existing general frequency/duty selection and timing behavior.

The packer detects shifts on every output rather than only output 1. If any
output is shifted, all outputs share the same phase origin, including repeated
and zero phases. Two distinct outputs with equal phases therefore receive
40 ns rising-to-rising and 20 ns rising-to-falling setup intervals.
Explicit matching SDC clocks remain allowed on zero-phase outputs; shifted
outputs must use derived phase constraints. Explicit inverse-buffer clocks
remain rejected when their source belongs to the shared phase group.

```sh
python3 mistral/tests/pll/phase_select.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-phase-select
```

The mandatory [reference bundles](fixtures/phase-select/) cover 0/0/270°,
0/270/90/180° and 0/180/180/0°. Each includes compressed Quartus RBF bytes,
checksums, portable inputs and fitter evidence. The runner checks complete
FPLL settings, counter-to-buffer mapping, all ordered clock pairs with both
capture edges, aligned-zero SDC acceptance and shifted/inverted SDC rejection.
The original 0/90/180/270° fixture remains a regression baseline.

No bitstream preset table, analog setting, clock route or Mistral source
changes are needed. This is host-only validation; physical phase accuracy
has not been measured. Downstream pins and hardware integration remain
separate work.


## Three/four outputs with 25 or 100 MHz references

Zero-phase multi-output profiles use the complete reference-specific feedback
and analog tuples in the table above. The selector tests every output frequency
and duty against one common 300/320/400 MHz configuration, preserving the
existing preference order. Packing and bitstream generation share this selector.
No new Mistral tables, BELs or routes are required.

```sh
python3 mistral/tests/pll/multi_reference.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-multi-reference
```

Six [portable Quartus oracle bundles](fixtures/multi-reference/) cover both
references with a 25/50/100 MHz triple at 50% duty (300 MHz), the same triple
at 25/50/25% duty (400 MHz), and a 40/80/16/20 MHz quad at 25/75/25/75% duty
(320 MHz). The runner compares every emitted FPLL setting, checks output timing,
buffer placement, mux selections and utilization, and rejects conflicting input
SDC and unsupported fine phase shifts at either new reference. `triple.py` and `quad.py` accept
`--reference-mhz` with a matching `--oracle-fixture`.

Shifted profiles use the separately checked reference/rate combinations below
and require 50% duty. The onboard DE10-Nano oscillator is 50 MHz; the new reference cases
are host-only checks and require an appropriate external clock for hardware
use. This extension adds no timing relationship to the input reference or
between unequal-frequency outputs. Downstream locks and FES pins are unchanged.


## Additional phase references and 50 MHz outputs

Two, three or four equal-frequency outputs support the following checked
quarter-cycle phase profiles, with 50 MHz eighth-cycle choices added below.
Output 0 remains at zero; the other outputs can
independently select any listed shift, including repeated and zero phases.

| Reference MHz | Output MHz | Allowed shifts (ps) | C divider | Counter presets | Phase mux presets |
| --- | --- | --- | --- | --- | --- |
| 25, 50, 100 | 25 | 0, 10000, 20000, 30000 | 12 | 1, 4, 7, 10 | 0, 0, 0, 0 |
| 50 | 50 | 0, 5000, 10000, 15000 | 6 | 1, 2, 4, 5 | 0, 4, 0, 4 |

These use the existing reference-specific 300 MHz feedback/analog tuples.
For 50 MHz outputs, the 90° and 270° shifts require `CNT_PH_MUX_PRESET=4`
as well as the counter preset; changing `CNT_PRESET` alone is insufficient.
The shared phase selector supplies both fields to bitstream generation and
the nanosecond shift to timing constraints. Mistral already describes these
fields; no Mistral table or clock-route change is needed.

```sh
python3 mistral/tests/pll/phase_rates.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-phase-rates
```

Nine [portable Quartus 17.0.2 oracle bundles](fixtures/phase-rates/) cover two,
three and four outputs for each new reference/rate combination. The runner
checks every emitted FPLL setting, clock buffers and muxes, utilization, all
120 ordered rising/falling capture crossings, phase-derived setup budgets and
Fmax. It rejects mixed output rates, unsupported phases/references/duties,
fractional feedback, contradictory input SDC and explicit shifted/inverted
output clocks. Existing zero-phase and 25 MHz phase profiles remain regressions.

All validation here is host-only. Non-50 MHz references cannot be tested on
the designated kit with its current clock source. The 50 MHz reference /
50 MHz output profile is suitable for a later ladder hardware experiment;
these compiler tests do not establish physical phase accuracy. Shifted 50 MHz outputs at 25/100 MHz references and unequal-frequency
phase relationships remain unsupported. The 100 MHz output extension below
adds another checked rate on the board reference. Downstream locks and FES pins are
unchanged.


## 100 MHz quarter-phase outputs

Two, three or four 100 MHz outputs from the 50 MHz V11 reference can use
0°, 90°, 180° or 270° phases (`0 ps`, `2500 ps`, `5000 ps`, `7500 ps`).
Output 0 stays at zero; all outputs must use 100 MHz and 50% duty.
The existing direct integer-feedback mode and output-buffer rules apply.

Quartus 17.0.2 uses the checked 300 MHz M12/N2 configuration with C3
dividers. The counter presets are 1/1/2/3, and phase-mux presets are
0/6/4/2. Odd-divider even-duty correction remains enabled. The nonzero
phase mux is essential: counter presets alone cannot encode these shifts.
No Mistral tables, BELs or clock routes are added.

The phase helper stores integer picoseconds, preserving 2500 and 7500 ps
through packing. Conversion to the existing timing API uses fractional
nanoseconds; Mistral's delay units are already picoseconds. The shared timing
algorithm is unchanged. Quarter-cycle setup windows are 2.5 ns, not rounded
to whole nanoseconds.

```sh
python3 mistral/tests/pll/phase_100.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-phase-100
```

The three [portable Quartus oracle bundles](fixtures/phase-100/) include full
compressed RBFs, hashes, fitter reports and regeneration inputs. The runner
compares every FPLL setting, checks utilization and dedicated clock-buffer
placement/muxes, and covers 40 ordered rising/falling capture crossings with
phase-derived timing budgets and Fmax. It rejects inexact shifts (including
2501 ps), out-of-range phases, mixed frequencies, non-50% duties, fractional
feedback, unsupported references and contradictory input/output constraints.

Validation is host-only; the 50 MHz board reference makes this profile
available for a later ladder hardware experiment. Physical phase accuracy
has not been measured. Shifted 100 MHz outputs with 25/100 MHz references
remain unsupported, as do unequal-frequency phase relationships. Existing
25/50 MHz phase profiles remain regression checks. Downstream locks and FES
pins are unchanged.


## 45-degree phase steps at 50 MHz

Two, three or four equal 50 MHz outputs from the 50 MHz V11 reference can
select phase shifts in 45° steps. Output 0 remains at zero; every other
output independently accepts `0 ps`, `2500 ps`, `5000 ps`, `7500 ps`,
`10000 ps`, `12500 ps`, `15000 ps` or `17500 ps`. Repeated and zero phases
are allowed. All outputs require 50% duty and direct integer feedback.

| Phase degrees | Shift ps | Counter preset | Phase-mux preset |
| --- | --- | --- | --- |
| 0 | 0 | 1 | 0 |
| 45 | 2500 | 1 | 6 |
| 90 | 5000 | 2 | 4 |
| 135 | 7500 | 3 | 2 |
| 180 | 10000 | 4 | 0 |
| 225 | 12500 | 4 | 6 |
| 270 | 15000 | 5 | 4 |
| 315 | 17500 | 6 | 2 |

Quartus 17.0.2 confirms the four new odd multiples of 45° at the existing
300 MHz M12/N2 configuration with C6 dividers (high/low 3/3). The previous
quarter-cycle entries remain unchanged. This extends the explicit checked
phase table; the packer, bitstream writer and timing analysis already consume
its picosecond offsets and counter/phase-mux presets. No Mistral tables, BELs,
clock routes or timing algorithm changes are needed.

```sh
python3 mistral/tests/pll/phase_45.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/pll-phase-45
```

Four [portable Quartus oracle bundles](fixtures/phase-45/) cover the new angles
across two, three and four outputs, including repeated zero phases and mixed
old/new angles. The runner compares every FPLL setting, verifies utilization
and dedicated clock-buffer placement/muxes, and checks all 64 ordered
rising/falling capture crossings against phase-derived setup windows and Fmax.
It rejects 2501 ps and other inexact/out-of-range shifts, unsupported references,
mixed frequencies, non-50% duties, fractional feedback and contradictory
input/output constraints.

Validation is host-only. The profile uses the board's 50 MHz reference and is
available for a later ladder hardware experiment; physical phase accuracy is
not established here. Shifted 50 MHz outputs still reject 25/100 MHz references.
The 25 and 100 MHz output profiles retain their checked quarter-cycle choices.
Downstream locks and FES pins are unchanged.


## Two independent PLLs on the board reference

Two `altera_pll` instances can share the DE10-Nano V11 50 MHz reference.
The packer first uses FPLL (0,14), then FPLL (0,31). Each cell retains its
own feedback configuration, output dividers and timing phase group. The
second site is restricted to a 50 MHz reference; this extension does not
add support for other physical reference sources.

Mistral already describes both dedicated input routes and their output
connections. No Mistral source or table changes are required:

| FPLL | V11 reference input | `CLKIN_0_SRC` | C6 destination | HG selector |
| --- | --- | --- | --- | --- |
| (0,14) | CLKIN0 | 4 | CMUXHG (0,35) PLLIN14 | 0x16 |
| (0,31) | CLKIN2 | 6 | CMUXHG (0,35) PLLIN6 | 0x0e |

The backend imports each counter's connections to all four CMUXHG lanes,
using Mistral's existing PLLIN selector encoding. Packing reserves a distinct
free lane for every output before binding a PLL. The previous lane preference
2/3/1/0 is preserved whenever those lanes are free; a second PLL uses remaining
lanes. Dedicated clock-buffer legality now checks the actual PLL-to-buffer
connection instead of requiring a fixed output number for each lane. Both
PLLs together can use at most four output lanes on this mux. A third PLL or
an output count exceeding the available lanes fails during packing.

```sh
python3 mistral/tests/pll/two_pll.py --yosys "$YOSYS" --nextpnr "$NEXTPNR" \
  --mistral-cv "$MISTRAL_CV" --output /tmp/two-pll
```

The main fixture combines a 25 MHz integer PLL and a 12.288 MHz fractional-N
PLL with independent counters and one HPS GP block. Two
[portable Quartus 17.0.2 oracle bundles](fixtures/two-pll/README.md) check
both assignments of these profiles to the two physical sites. The runner
verifies their compressed reference hashes and compares all emitted FPLL
settings, including the auxiliary bandgap. It also checks the OSS mux
selectors, utilization, compressed RBF generation and output timing.
Quartus may choose a vertical clock mux for one output; the oracle comparison
covers FPLL settings, while OSS horizontal mux selections are checked against
the imported Mistral connections.

An observed data crossing between the two PLL outputs remains cross-domain,
including when the requested output frequencies are equal. A common reference
does not establish a synchronous timing relationship between separate PLLs.
The runner also exercises all four shared lanes and rejects a fifth output,
a third PLL and a conflicting reference constraint.

This change is based on nextpnr fork `mistral-stable` commit
`1dad4cc2ea75944b0e3b645cf9e82dd88b6a9faa`, with unchanged Mistral
`78ba2a580ae2523403d4f4f91891a6b11d7b6aba` and Yosys
`13b43f8c85ec430a33ee55d058fb4c32b42b6910`. Validation is host-only; neither
this test nor an RBF build programs hardware. Physical lock, frequency and
phase behavior at the second site remain for ladder hardware acceptance.
No misteross lock or FES parent pin changes are included.
