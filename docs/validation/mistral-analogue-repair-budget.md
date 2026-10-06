# Bounded Mistral analogue repair

The placement study retained two seed-4 timeouts after analogue timing had
started. Repair saved its best routing, but a nested GPU-router call could
spend the remaining compiler time in congestion negotiation. External process
termination prevented restoration and final timing of that saved routing.

Mistral now gives analogue repair a cooperative five-minute budget. Use
`--gpu-opt analogueTimeBudget=SECONDS` to override it; `0` disables the limit.
Initial routing is unchanged. The first analogue timing always completes so
repair has a measured route to retain. Candidate generation, candidate scoring
and nested routing check the budget at safe boundaries. Active backend batches,
graph setup steps and timing analyses finish before cancellation; this is
not a hard deadline for the whole compilation.

Cancelled nested routing unwinds its Context lock and backend state before
the caller restores the best saved routing. Cancelled candidate selection
also restores the temporary search delay prior. Partially changed candidates
are never treated as already timed. Restored routing is configured and timed
again through the existing final analogue setup/hold gate. A requested
optimization margin remains distinct from timing acceptance.

## Validation

Two new unit tests check cancellation inside the locked negotiation loop,
candidate cancellation, invalid budgets and the disabled limit. The 31
existing IO timing regressions also pass.

`mistral/tests/analogue_budget.py` uses a retained passing menu DDR checkpoint
and an unattainable repair target to exercise these cases:

| Case | Result |
| --- | --- |
| Repair disabled | ordinary final timing passes |
| Immediate budget expiry | final timing passes; exact baseline bindings and RBF retained |
| Nested rerouting before expiry | best routing retained or restored; final timing passes; checkpoint reproduces the exact resulting RBF |
| Candidate selection before expiry | best routing retained or restored; final timing passes; checkpoint reproduces the exact resulting RBF |
| Immediate expiry with an impossible clock constraint | final analogue setup fails; compiler exits 1 |

Later expiry can retain an improved completed round, so its bitstream need
not match the initial one. The test checks replay of the returned routing
rather than rejecting a legitimate improvement. The failing-clock test still
has positive hold slack and exits unsuccessfully because setup is negative.
The exact stopping boundary depends on host speed: a round may complete just
before expiry. Results record whether expiry occurred between rounds, during
candidate selection or during rerouting; in-progress cancellation must log
restoration of the best saved routing.

To reproduce with the checked-in menu synthesis fixture:

```sh
python3 mistral/tests/gpurouter/menu_ddr_seed1.py \
  --nextpnr build/nextpnr-mistral --gpu-cpu --output /tmp/menu-budget-base
python3 mistral/tests/analogue_budget.py \
  --nextpnr build/nextpnr-mistral \
  --checkpoint /tmp/menu-budget-base/routed.json --output /tmp/menu-budget-check
```

A separate immediate-expiry replay of the previously hardware-tested
100 MHz SDRAM checkpoint also passes final timing and preserves its exact RBF
SHA-256:
`d65bb881197b03d70495105ac0d59a16a46414785f085f91f19f8d2b664ab641`.
No new hardware run or full SDRAM board timing qualification is claimed.
Commands, inputs, reports and logs were retained locally; their hashes and
results are in
[analogue-budget-reference.json](../../mistral/tests/ramtest-io/analogue-budget-reference.json).

Validation used the CPU reference backend. Cooperative checks surround the
same backend API for device routing, but a GPU device run has not been made.
The earlier seed-4 fresh compilation was not repeated; these checks exercise
the cancellation and restoration paths using routed checkpoints.
