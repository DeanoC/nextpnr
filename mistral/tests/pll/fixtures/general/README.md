# General-solver end-to-end Quartus references

Quartus Prime Lite 17.0.2, `5CSEBA6U23I7`, generic `altera_pll` with
`PLL_COMPENSATION_MODE DIRECT`. Each case directory holds the design
(`top.v`), the pins and SDC shared by both tools (`pins.qsf`, `clocks.sdc`),
nextpnr's placement (`placement.txt`: PLL instance, FPLL x/y, output
counters), the Quartus project that pins the same FPLL sites and
`PLLOUTPUTCOUNTER` locations (`top.qsf`), the compressed Quartus RBF
(`top.rbf.gz`, gzip mtime 0) with `sha256.txt`, the decoded FPLL/CMUX
settings and inversions (`pll-settings.txt`) and the fitter's PLL Usage
Summary (`fitter-pll.txt`). Quartus chooses its own global clock lanes; only
FPLL settings are compared. Host-only references: no hardware was programmed.

Regenerate a case with `../../solver_tools/make_general.py <case>` (set
`QUARTUS_BIN`, `YOSYS`, `NEXTPNR`, `MISTRAL_CV`). Round-tripping each
Quartus RBF through `mistral-cv decomp`/`comp`/`diff` differs only in a few
fabric CRAM bits, so the decoded FPLL settings cover every PLL bit Quartus
sets.
