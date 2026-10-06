# SDRAM issue #135: resolution and Atari ST handoff

Issue [#135](https://github.com/DeanoC/nextpnr/issues/135) reported a 100 MHz
RAM tester that passed internal timing but failed SDRAM on hardware. Its SDC
constrained only the 50 MHz input clock. Address launches through fabric FFs
were therefore unconstrained, and changing the netlist or placement could
produce a hardware failure without an internal timing failure.

The production RAM tester already received command/address IO registers in
[FES #524](https://github.com/DeanoC/fes/pull/524), merged as `92933f80`.
That fix passed Kit B HIL and returned ramtest to the shared toolchain lock.
The issue was subsequently kept open for external IO delay support and
bidirectional DQ packing. Nextpnr #122/#123 supplied the packing; this change
supplies `set_input_delay`/`set_output_delay`, their timing boundaries,
checkpoint persistence, diagnostics and regression coverage.

## Evidence for closure

- A controlled reroute changed only A9/A10/A12 in the original failing design.
  The same-boot hardware comparison gave 128 errors, then zero, then 160 when
  the original failing control was restored. Logic, placement and other routes
  were held fixed. This demonstrates an address-routing cause for that artifact.
- Fresh pack/place/route of the original synthesis with targets on all thirteen
  address outputs passed hardware twice around the failing control: 0/228/0
  errors. No RTL change, frozen placement or selected routes were used.
- A declined analogue arc override was found to contaminate fallback route
  minima with zero. Fixing the fallback gives a fresh compiler exit 0 with
  memory setup/hold of +0.032/+0.418 ns. Its RBF is byte-identical to the
  hardware-tested fresh build.
- Additional analogue repair produces +0.435/+0.418 ns memory setup/hold and
  another same-boot hardware result of 0/480/0 errors, with all six SDRAM
  patterns completed and HPS DDR passing on all three loads.

The [retained investigation](../../mistral/tests/ramtest-io/README.md) contains
the receipts, package/RBF identities, screenshots and reproduction commands.
[IO-delay documentation](../mistral-io-delay.md) defines supported constraint
semantics and model qualification. The empirical address target is a native
GPIO arrival target for the historical fabric-register design; it is not a
complete SDRAM chip-pin constraint and must not be copied into another core
as a board timing specification.

## Atari ST handoff

The compiler support is reusable by the Atari ST work without waiting for
130 MHz or timing closure across every placement seed. Use the merged IO
packing and inspect the actual packed input/output/OE registers. Derive that
core's external constraints from its emitted SDRAM clock, capture edges,
controller sequence and board assumptions. Retain related PLL clocks in the
same timing relationship; asynchronous clock groups must not cut the intended
SDRAM checks. Run the ordinary timing gate and the Atari ST hardware regression
with its own byte-mask/read/write checks. RAM tester acceptance does not certify
Atari ST issue #125 or substitute for its hardware test.

## Follow-up work outside issue #135

- Characterize board/package/PVT bounds and complete DQ, command, OE turnaround
  and forwarded-clock coverage. The opt-in Quartus pad profile is a fitted
  reference envelope, not complete board signoff.
- Improve internal placement/routing consistency. In the bounded 500 ps-target
  study seed 2 passed at 435 ps, seed 3 failed internal setup, and seed 1 timed
  out. The requested 500 ps target was not achieved.
- Validate 130 MHz in OSS against the hardware-passing Quartus reference.
- Integrate any new constraints into the current FES production recipe with a
  fresh producer package and HIL; the historical experimental packages do not
  replace the already merged production IO-register fix.

Closing #135 acknowledges the original hardware fix and the requested compiler
capabilities. It does not claim these follow-ups or Atari ST acceptance are done.
