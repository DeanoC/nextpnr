# Mistral LAB FF reset recovery reference

By default `MISTRAL_FF.ACLR` retains the historical synchronous-data setup/hold
approximation (-196/270 ps). Passing those checks does not establish reset
recovery/removal, pulse-width or metastability qualification.

`--mistral-ff-reset-reference` opts into a bounded Quartus 17.0.2 local clear
reference on `5CSEBA6U23I7`. It uses 170 ps recovery in the setup equation and
retains the existing 270 ps hold floor for removal. Ordinary data/control
checks are unchanged. The [characterization fixtures](../mistral/tests/ff-reset/README.md)
retain 1,648 four-corner observations, including every ALM FF position and
constant, dynamic and inverted enables.

The option requires an exact rising physical clock waveform: period >=10 ns,
high/low >=5 ns, a driven active-low clear without inversion, enabled or driven
dynamic ENA, and inactive SCLR/SLOAD. Unsupported device, clock or control
configurations fail explicitly when the clear timing model is requested.
The setting is saved in checkpoints; reloading requires the same CLI option,
so a saved reference cannot silently fall back to weaker default recovery.

No false path, clock cut, multicycle allowance or failure override is added.
Asynchronous/unrelated launch paths remain unpaired. Reset pulse width,
clock/clear slew, fast-corner routing, reset-chain topology and consumers,
PLL lock/relock and MTBF remain separate physical obligations. This mode is a
stricter reference screen, not whole-board or silicon signoff. Registered GPIO
clear models are unaffected and retain their existing unsupported boundary.

Calibration queries for disconnected clears with the explicit packed inactive
`PIN_1` state retain the ordinary data approximation; no reset path exists on
those ports. Missing or undriven signal clears remain unsupported.
