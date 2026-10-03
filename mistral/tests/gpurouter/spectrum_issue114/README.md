# Spectrum GPU routing plateau qualification (nextpnr issue 114)

These fixtures qualify the initial-routing congestion policy fixed by
`53e1ad42b3fc3bb016a6dbbddfe3fc292f98b98c` for
[nextpnr issue 98](https://github.com/DeanoC/nextpnr/issues/98).
[Issue 114](https://github.com/DeanoC/nextpnr/issues/114) supplies two
Spectrum cases observed on the older HIP nextpnr `0259c6dc` stack. They are
qualification inputs; neither establishes a failure in a current build.

| Directory | Immutable publication | FES source | Default seed | Evidence |
| --- | --- | --- | --- | --- |
| `retained/` | [Original inputs and logs](https://gist.github.com/DeanoC/e5261a539b56d1be3e7618238c9b7d6d/cda29dcb208ef61bc3e705c1d81dc0934cb21ac5) | `792b24805ce714e890485c71fe122ebf0e445c8e` | 4 | Original synthesis, constraints and complete producer logs |
| `reconstructed/` | [Reconstructed inputs and captured excerpts](https://gist.github.com/DeanoC/ec89a958aa4d25d4e3ef1bfb8a221ed1/d8c25681be29bfda1e56a09a8cac990f49939f62) | `deb0f18d53467083845c0d7a5f4b1d9b7876d4e4` | 2 | Reconstructed synthesis with matching recorded execution fingerprint; original synthesis digest and complete logs unavailable |

Start with `retained/`: seed 4 failed at iteration 104 with one overused
wire, `TD.10.44.20`, shared by `gp_mailbox.unit0_size[11]` and `[7]`.
The reconstructed case's captured seed 2 excerpt records a one-wire
plateau at iteration 122. Its captured seed 4 excerpt records a producer
timeout after 600 seconds during routing. Packaging these artifacts did
not run synthesis or routing.

The retained `SOURCE-README.md` says seed 5 completed and final analogue
timing was recorded. The complete `seed-5-route.log` instead ends with a
600-second producer timeout during reversed timing repair. It reaches
zero overuse at iterations 158, 193 and 202, but contains no successful
tool-completion tail or final analogue clock results. Preserve that
distinction when reporting results. Seed 4's `Routed Fmax (pip delay
table)` values are also provisional, not final analogue timing.

Both inputs contain sixteen firmware `MISTRAL_M10K` lanes with all-zero
10,240-bit `INIT` parameters and `CFG_ASYNC_READ=1`. A maintained build may
reject those async-read lanes during packing. Classify that result as
input compatibility, preserve the original fixture, and do not silently
alter its primitives to make a route run. The clock requirements remain
52.224 MHz system, 74.25 MHz pixel and 12.288 MHz audio. Diagnostic
`--timing-allow-fail` does not waive FES timing or package sealing gates.

## Files and verification

Each fixture's `fixture.json` records the default seed, exact QSF filename,
and hashes for compressed synthesis, decoded synthesis, QSF and SDC.
The compressed files preserve the publication's gzip bytes: only the
base64 transport wrapper was removed. No mapped TOP primitive names,
parameters, port directions or connectivity changed during import.

`SHA256SUMS` covers every stored file in its fixture directory except
itself. Verify from that directory with `sha256sum --check SHA256SUMS`.
The runner additionally verifies the decoded synthesis hash.

The publication's `README.md` and `SHA256SUMS` are retained byte for byte as
`SOURCE-README.md` and `SOURCE-SHA256SUMS`; `provenance.json`, logs,
`build-inputs.json` where supplied, QSF, SDC and `COPYING` are also retained
unchanged. Names in `SOURCE-SHA256SUMS` describe the original publication:
its `README.md` refers to local `SOURCE-README.md`, `synth.json` refers to
the decoded gzip content, and `synth.json.gz.b64` was checked before its
transport wrapper was removed. The original base64 hash remains in
`fixture.json`. Use the new `SHA256SUMS` for the stored files; historical
base64 decode commands in `SOURCE-README.md` describe the publication.

All original publication checksums were verified before import. The
original unsanitized synthesis and log hashes, where available, remain
in `provenance.json`; those original unsanitized files are not supplied.
Source/tool attribution paths were already sanitized by the publications.

## Source and licensing

The Spectrum shell and mailbox are GPL-2.0-or-later; the original FES Z80
engine is MIT. Corresponding source and SPDX notices are at the immutable
FES source links for
[retained](https://github.com/DeanoC/fes/tree/792b24805ce714e890485c71fe122ebf0e445c8e/sources/misteross)
and
[reconstructed](https://github.com/DeanoC/fes/tree/deb0f18d53467083845c0d7a5f4b1d9b7876d4e4/sources/misteross)
inputs. Each directory preserves the publication's GPLv2 `COPYING`.
Generated Intel ALM primitive sources and license notices are at
[Yosys e2d425de](https://github.com/DeanoC/yosys/tree/e2d425dee148cc60c50f4e9b354a10d90eab15f4/techlibs/intel_alm).

## Replay

Build baseline `0259c6dc1c46dd46fe79f3923a17ad36d2513421` and that revision
with only `53e1ad42b3fc3bb016a6dbbddfe3fc292f98b98c` backported. Use the same
compiler, Release configuration, Mistral
`7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039` and HIP architectures
`gfx1100;gfx1201` for both. Current-head packing rejection does not exercise
the historical routing policy. The fixture is already mapped; no Yosys run
or regenerated BUILD_ID is needed.

Run from the nextpnr source root with fresh output directories:

```sh
# Check that the baseline reproduces the documented initial plateau.
python3 mistral/tests/gpurouter/spectrum_plateau.py \
  --nextpnr build-baseline/nextpnr-mistral --fixture retained \
  --expect-failure plateau --output /tmp/spectrum-original

# Qualify the isolated fix and compare two complete runs per fixture.
python3 mistral/tests/gpurouter/spectrum_plateau.py \
  --nextpnr build-fixed/nextpnr-mistral --fixture retained \
  --repeat 2 --output /tmp/spectrum-fixed-retained
python3 mistral/tests/gpurouter/spectrum_plateau.py \
  --nextpnr build-fixed/nextpnr-mistral --fixture reconstructed \
  --repeat 2 --output /tmp/spectrum-fixed-reconstructed

# Run the checks for partial convergence, timeouts and invalid evidence.
PYTHONDONTWRITEBYTECODE=1 python3 mistral/tests/gpurouter/spectrum_plateau_test.py
```

The default seeds are 4 for retained and 2 for reconstructed. Each command
uses Cyclone V `5CSEBA6U23I7`, HeAP timing weight 2000, criticality exponent
5, GPU device 0 and a configurable 1,800-second timeout. Default runs
require an actual HIP backend; `--gpu-cpu` explicitly selects and checks
the CPU reference backend instead. Each invocation authenticates the
compressed/decoded synthesis and exact constraints before routing, writes
per-run logs, and persists `summary.json` after each attempt, including
failures. Existing output roots are refused to prevent stale evidence.

A routing pass requires successful tool completion, legal complete
routing, zero overuse, a logged checksum, nonempty routed JSON/RBF/timing report,
and final analogue evidence for all three expected clock domains. Expected
clock constraints are compared as periods within 1.01 ps: Mistral stores
whole-picosecond periods, and report conversion adds float rounding. Timing
closure is recorded independently; a legal route with a timing miss is
not a routing failure. Repeats must match the last logged routing checksum,
final RBF SHA-256 and final analogue results. Analogue repair can restore a
different candidate after the logged checksum, so the RBF hash describes
the final emitted configuration. `--expect-failure plateau` passes only when
the old initial-negotiation failure is reproduced; it does not declare a
successful route. `--expect-failure packing` checks the asynchronous-M10K
rejection separately. Timeouts cannot satisfy either expected failure.

## Qualification results

The comparison uses baseline `0259c6dc` and backport commit
`fdb8e11c57a7c4e1afa83f1864fc57f8dedaa08c`, whose patch ID equals the
original `53e1ad42` change. Both link the same authenticated original
Mistral archive and the same HIP archive rebuilt from unchanged pinned
sources with the original compiler/settings. The original HIP archive
had been cleaned away. This reuse avoids regenerated native-struct
padding differences in Mistral's embedded device data; actual data fields
were also checked to match. The build metadata records exact binaries,
sources, compiler hashes, arguments, common libraries and relink steps.

HIP GPU 0 was the AMD Radeon RX 7900 XTX (`gfx1100`). Both the original
cached binary and rebuilt baseline reproduce all 104 iteration lines of
the retained seed 4 producer log exactly, including its shared unreserved
wire and exit 125. The reconstructed seed 2 baseline reproduces the
retained diagnostic lines verbatim, including iteration 122 and exit 125.
These are reproduced failures, not timing results from valid routes.

| Fixture / seed | Baseline | Fixed initial zero overuse | Fixed complete runs | Final analogue MHz (system / pixel / audio) |
| --- | --- | --- | --- | --- |
| Retained / 4 | Exit 125 at iteration 104 | Iteration 137 | Two; identical final RBF and timing | 52.8961 / 96.0246 / 207.3828 |
| Reconstructed / 2 | Exit 125 at iteration 122 | Iteration 143 | Two; identical final RBF and timing | 52.2111 / 80.3084 / 194.7420 |

All four fixed attempts complete with zero overuse, successful legal
routing, exit 0, final analogue reports and nonempty outputs. Repeats
match the logged checksum, final RBF SHA-256 and final analogue timing.
The retained case passes all three timing gates. The reconstructed case
misses the 52.224 MHz system requirement at 52.2111 MHz; pixel and audio
pass. This is a routing-convergence qualification with a separately
recorded timing miss, not a sealed FES package.

The retained fixed runs initially exposed an overly strict MHz comparison
in the runner: the valid reported system constraint is 52.224773 MHz
after whole-picosecond period conversion. The corrected period check
reassessed the saved outputs without rerouting or changing artifact
bytes; both compiler runs had already completed with exit 0. The prior
harness summary, corrected assessment and artifact hashes are retained.

[Machine-readable qualification](evidence/qualification.json) links the
complete raw logs and summaries, exact backport, build provenance,
padding audit and policy-test results. `evidence/SHA256SUMS` authenticates
all stored evidence files. Large binaries, source archives, generated
build files and routed outputs remain in the task's qualification
directory; their paths and hashes are recorded in the evidence.

The separate cached `655f3833` compatibility check exits 125 during
packing on `CFG_ASYNC_READ=1`, before any GPU negotiation. The current
source retains that rejection. These are historical compiler-routing
qualification results; no hardware or FES package-sealing run was made.
