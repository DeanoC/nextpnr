# 45-degree steps at 50 MHz

Quartus Prime Lite 17.0.2 host-only references on `5CSEBA6U23I7`.
All use the 50 MHz board reference, 50 MHz outputs, 50% duty, and direct integer
feedback. No hardware has been programmed or measured for these bundles.

| Profile | Output phases (ps) | Counter presets | Phase mux presets |
| --- | --- | --- | --- |
| dual315 | 0 / 17500 | 1 / 6 | 0 / 2 |
| triple225 | 0 / 0 / 12500 | 1 / 1 / 4 | 0 / 0 / 6 |
| quadOdd | 0 / 2500 / 7500 / 12500 | 1 / 1 / 3 / 4 | 0 / 6 / 2 / 6 |
| quadMixed | 0 / 5000 / 15000 / 17500 | 1 / 2 / 5 / 6 | 0 / 4 / 4 / 2 |

Every profile selects M12/N2, VCO 300 MHz, C6 on each active output,
and high/low dividers 3/3. Output indices map to C6/C7/C5/C8.
The four bundles verify the newly supported 45/135/225/315-degree values;
existing phase-rates fixtures cover 0/90/180/270 degrees.

Each subdirectory includes portable inputs, the exact compressed Quartus RBF,
raw/compressed SHA-256 hashes, and fitter/decompiled PLL settings. See its
README for regeneration commands and the scope of the recorded evidence.
Full FPLL comparison must decode the RBF to include default settings.
