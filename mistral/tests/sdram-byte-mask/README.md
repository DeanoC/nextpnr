# SDRAM DQM pad and synchronous-control isolation

This host fixture addresses [nextpnr #125](https://github.com/DeanoC/nextpnr/issues/125).
It puts separate lower/upper mask registers on AG13/AF13, the actual SDRAM
DQM pads, and pins the native fabric registers to the sealed ST shell's
LAB 42,3 top/bottom halves. It does not contain a CPU or an SDRAM controller.

The `sclr` case uses a shared synchronous clear, exercising the backend's
forced-disabled SLOAD workaround. The `plain-d` case computes the equivalent
`data & !clear` function with explicit LUTs and inactive dedicated controls.
Explicit primitives prevent synthesis from turning that comparison back into
the same control form. `zero` and `one` check static driving of both pads.

`oracle.v` implements the same two control forms with Quartus `dffeas` fabric
registers. The oracle disables FAST_OUTPUT_REGISTER for both DQM pads and
verifies two logic registers and zero I/O registers. The compiler comparison
uses the same input clock edge, pads and output-register class; placement and
clock distribution are compiler-specific. It is not external timing closure.

```sh
python3 mistral/tests/sdram-byte-mask/check.py \
  --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --output /tmp/sdram-byte-mask \
  --quartus-root /path/to/quartus
```

Quartus is optional. The output retains synthesis/routing JSON, RBFs, decoded
settings, compiler logs and `results.json` with tool and artifact hashes.
The checks include actual pad bindings, live output routes, constants,
output-enable and data inversion, distinct FF control forms, and matching
native/Quartus GPIO drive settings.

On the issue's source revisions (nextpnr `3d4a5b35`, Yosys `886afa63`,
Mistral `7ed06e21`), all four native cases and both Quartus 17.0.2 cases
compile. Both compilers select the same DQM drive strength, disable the input
buffer and drive the outputs. Native additionally selects
`INPUT_REG4_SEL=SEL_LOCKED_DPA` for the two DQS lanes; Quartus leaves the
database's `SEL_BYPASS` default. This input-path difference is retained for
investigation, not classified as an output fault. Both shared-clear variants
set per-register SLOAD enable with global SLOAD disabled, consistent with the
backend workaround; Quartus chooses DIN1 for SCLR while native routes DIN3.

The independent byte-preservation diagnostic belongs in FES
`sources/misteross/cores/fes-ramtest`, using the existing controller's
BYTE_MASK_ENABLED path. Its host simulation and a decoded RBF do not prove
physical SDRAM acceptance. The next isolation is a matched RAMTEST run or
measurement of the actual DQM pads during WRITE. No hardware is programmed
by this fixture, and no compiler defect is established here.
