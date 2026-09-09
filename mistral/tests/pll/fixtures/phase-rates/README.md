# PLL reference and phase-rate oracles

These portable Quartus Prime Lite 17.0.2 bundles cover two independent additions:

- Two, three and four 25 MHz outputs with quarter-cycle shifts, using 25 MHz
  or 100 MHz input references.
- Two, three and four 50 MHz outputs with quarter-cycle shifts, using the
  board's 50 MHz input reference.

All profiles use integer feedback, direct operation, 50% duty and device
`5CSEBA6U23I7`. Output indices occupy C6/C7/C5/C8 in order. Two-output cases
use phases 0/270 degrees, three-output cases use 0/0/270 degrees, and
four-output cases use 0/90/180/270 degrees.

The 25 MHz outputs use VCO300 MHz and divide12. The 50 MHz outputs use the
same VCO and divide6: 90/270 degrees require phase mux4, together with counter
presets2/5. Each bundle contains its fitter report and exact compressed RBF;
comparisons must load the RBF to include default settings.

These are host-only oracle results. Non-50 MHz references need an external
clock source and cannot be tested with the current kit setup. No fixture was
programmed into hardware. See each bundle's README for regeneration commands,
configuration details and checksums.
