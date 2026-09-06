# Cyclone V integer PLL

This is bounded support for single-output integer PLL configurations and
the dual-output profile described below. The HPS diagnostic has
measured the expected output/reference frequency ratio and asserted lock on
hardware. It is not general-purpose PLL support or system-image acceptance.

The test uses the existing Yosys `altera_pll` blackbox, without a Yosys patch.
One physical FPLL is reserved per cell. On `5CSEBA6U23I7`, Mistral enumerates
six FPLL sites. The currently accepted configuration is:

- Reference: 25, 50 or 100 MHz from the dedicated V11 input route.
  The DE10-Nano onboard oscillator supplies 50 MHz; other references require
  an appropriate external physical clock source.
- Output: one clock from 1 to 100 MHz, expressed as decimal MHz with
  at most one-Hz precision and an exact C divisor
  from the checked 300/320 MHz reported VCO configurations; zero phase, 50% duty.
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
the dedicated CLKIN.0 input links, and C6 links to CMUXHG PLLIN ports.
Single-output profiles use CMUXHG subblock 2; the dual profile also exposes
subblock 3 for its dedicated second output.

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

`rst` maps to Mistral's existing FPLL `NRESET0` fabric endpoint. Quartus17's
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
50 MHz reference to 12.288 MHz single-output profile. Other fractional-N
reference/output combinations and multiple outputs are rejected. The existing
V11 route, direct mode, zero phase, 50% duty and reset rules still apply.
The default `"false"` mode retains exact integer-divider behavior.

Quartus17.0.2 selects M8, N1 (bypass), C6=33 and fractional word
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
