# Experimental coverage for unobserved arithmetic outputs

`--critical-unobserved-arith-outputs` is a separate, default-off coverage option
for a positive `--critical-private-pair-budget` or `--critical-lut-permute-budget`.
Those repairs retain their fresh full GPU route, RBF and ordinary execution
requirements. The flag does not enable a repair by itself, does not apply to
LUT6 swaps, and is never enabled by saved settings. The existing strict output
coverage API and its callers retain their behavior.

The optional walker may admit an unobserved `MISTRAL_ALUT_ARITH` SO or CO output
only after traversing a characterized finite native arithmetic input-to-output
arc. An empty starting net always rejects. Every sibling output is still
visited, and the complete starting cone must reach at least one FF or M10K
capture with all native setup and hold relationships validated. An entirely
unobserved cone rejects. No timing row or margin is invented for an empty output.

An admitted leaf must be the canonical live net and cell with the exact output
port and driver backreferences, `PIN_SIG`, native combinational-output timing
class and no associated clock. It must have zero active indexed users and zero
routed wires. Public, global, clock, constant, region, protected and FES-boundary
identities reject. Legitimate arithmetic carry clustering is allowed; this is a
structural observer check and does not move or reconfigure the carry cell.

Zero indexed users alone is insufficient. On the first optional leaf the walker
counts every nonnull live cell-port reference, including input, inout and output
ports, then requires exactly the one already validated driver reference for each
leaf. This catches an observer missing its reciprocal user entry and duplicate
output identities. Top-level ports are checked independently. The lazy scan is
bounded to 1048576 live cell-port entries per invocation; it does not consume the
separate 4096-node cone-work bound. Existing depth, cycle, unsupported capture,
clock-cut, asynchronous-control and hybrid-path rejections remain intact.

Both repairs select this wrapper before each trial and after actual local
routing in skewed and reference timing frames. Their source-origin checks,
stationary route retention, physical legality, atomic rollback and whole-design
clock/endpoint guards are unchanged. Every known positive or negative setup and
hold margin must remain nonregressing, with at least 20 ps improvement to an
existing setup failure. Structural leaf eligibility is not routed timing closure,
external-IO qualification or hardware acceptance.
