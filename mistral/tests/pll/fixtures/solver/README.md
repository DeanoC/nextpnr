# Quartus 17.0.2 PLL solver oracle

39 Quartus Prime Lite 17.0.2 compiles for `5CSEBA6U23I7` with up to six
generic `altera_pll` instances each (direct compensation, each PLL on its own
dedicated reference pin and reset), used to derive and check
`mistral/pll_solver.h`. `runs/<run>/` keeps the generator spec (`spec.json`) and portable inputs
(`top.v`, `top.qsf`, `clocks.sdc`), the fitter's PLL Usage Summary and the decoded
FPLL/CMUX settings of each compile; `rbf-sha256.txt` identifies the RBFs.

`cases.txt` has one line per oracle PLL: reference, fractional mode and the
requested outputs, followed by the Quartus results the solver must
reproduce (reported VCO, M, N, fractional word, BWCTRL, CP_CURRENT, M
presets, VCO_DIV, M odd-duty and N bypass bits, and per output the counter
divider, high/low counts, odd-duty and bypass bits and phase presets).
`garbage` marks requests Quartus mis-implements (the solver must reject
them). `../../solver.py` replays the file.

Regenerate with `../../solver_tools/pllgen.py runs/<run>/spec.json <dir>` (or
`batch.py`) and `export_fixture.py`. Host-only evidence; no hardware was programmed.
