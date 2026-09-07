# 100 MHz quarter-phase references

Quartus Prime Lite 17.0.2 oracle bundles for two, three and four 100 MHz
outputs from a 50 MHz reference. All use direct integer feedback and 50% duty.
The profiles exercise a late shifted output, repeated zero phases and all four
quarter phases. Counter ordering matches the nextpnr C6/C7/C5/C8 mapping.

Each directory includes portable reproduction inputs, the compressed reference
RBF, hashes, complete fitter PLL usage and non-default FPLL/CMUX decomposition.
These are host-only reference builds; they establish configuration agreement,
not measured output frequency, duty, phase or jitter on hardware.
