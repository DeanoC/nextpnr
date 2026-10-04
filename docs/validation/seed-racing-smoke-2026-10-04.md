# Seed-racing CUDA build and bounded smoke validation

Date: 2026-10-04

This is host evidence for the draft seed-racing foundation, not a benchmark or
hardware acceptance result. The exact Catch and Pong inputs from issue 119
were not available, so that report remains unreproduced.

## Build

The isolated checkout was built with GCC 13.3, CMake 3.28.3, Ninja 1.11.1,
CUDA 13.3 for `sm_86`, and Mistral
`7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039`:

```sh
cmake -S . -B build-mistral-cuda -GNinja \
  -DARCH=mistral -DMISTRAL_ROOT=/absolute/path/to/mistral \
  -DGPU_ROUTER=CUDA -DCMAKE_CUDA_ARCHITECTURES=86 \
  -DBUILD_TESTS=ON -DBUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-mistral-cuda --parallel 8
```

The resulting `nextpnr-mistral` used an NVIDIA GeForce RTX 3090. The complete
test-enabled build exposed two telemetry integration defects that the earlier
standalone writer test could not instantiate: a string passed through the
numeric `Context::setting` template, and use of the RNG's warmed internal state
as though it were the requested seed. Both were fixed. Telemetry-only settings
are now installed after JSON import so they do not shift the imported design's
interned identifiers.

## Four-run smoke limit

Exactly four PNR invocations used the retained mixed-width M10K fixture, seed
1, CUDA device 0, and a 120-second per-run limit. Two ran with telemetry off
through `mistral/tests/gpurouter/qor.py`; two ran with telemetry on through the
bounded collector with concurrency one. Wall times were 8.3 seconds for the
first telemetry-off run and 8.1/8.2 seconds for the telemetry-on runs.

All four completed legal routing and passed final analogue timing for the one
required clock: 398.09 MHz achieved at 50.00 MHz, with reported worst setup
slack +17.488 ns. Their compressed RBFs were byte-identical, SHA-256
`cbe9d36c182491ae78c70cc2e4cc43c7c89d08ec3609032d813c1aeb1c6bcc2b`,
and their final reports were byte-identical. Both telemetry files contained 20
schema-v1 records, passed strict prefix/order/nesting validation, and had the
same non-time/run-ID fields.

The QoR wrapper returned failure after its two successful tool executions
because `--expect-clock FPGA_CLK1_50` did not match Mistral's canonical report
name `FPGA_CLK1_50_MISTRAL_IB_PAD_O_MISTRAL_CLKBUF_A_Q`. This was an assertion
argument error, not a route or timing failure.

The smoke binary predated the final seed/identifier-order correction. It
therefore recorded the warmed RNG state in `run_start.seed`, and telemetry-on
route checksums differed from telemetry-off checksums because the telemetry
setting key had been interned before design import. Same-mode repeats matched,
and the final RBF and analogue results matched across modes. The corrected
binary was rebuilt after the four-run limit was reached; a no-pack/no-place/
no-route serialization check preserved the maximum `uint64_t` requested seed
`18446744073709551615` exactly. No fifth PNR run was made.

## Tests and limits

- `nextpnr-heap-control-set-test`: passed.
- `nextpnr-gpuroute-telemetry-test`: passed (2 tests).
- `mistral/tests/seed_racing_test.py`: 14 passed.
- `mistral/tests/gpurouter/spectrum_plateau_test.py`: 8 passed.
- The monolithic `nextpnr-mistral-test` started 220 tests; its first 81 tests
  passed, then the process was OOM-killed after 343.57 seconds at about 22.7
  GiB RSS on a 23 GiB host with no swap. This was not an assertion failure.

The next smallest issue-119 experiment still requires its exact mapped Catch
and Pong inputs, constraints, tool revisions, options, and environment. Run
two same-seed telemetry-off and two telemetry-on replicates there, then compare
canonical final routing plus every required analogue setup/hold result; do not
use raw ID-based checksums alone across instrumentation modes.
