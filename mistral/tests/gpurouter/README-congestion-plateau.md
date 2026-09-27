# GPU congestion plateau regression

The standalone test executes the production stopping policy without a chip
database, GPU, or nextpnr build:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -I . \
    mistral/tests/gpurouter/congestion_plateau.cc \
    -o /tmp/nextpnr-congestion-plateau-test
/tmp/nextpnr-congestion-plateau-test
```

Run from the nextpnr source root. The test supplies occupancy sequences to
check that initial negotiation retains its configured iteration budget beyond
both early-exit conditions. It also checks the frozen-repair shortcuts,
counter resets, and eligibility reset between negotiation attempts.

Before the frozen-at-entry guard, four initial-routing cases fail. The
triggering RAM-test route reached one unreserved shared wire, with no frozen
arcs, and aborted after iteration 67 because the thirty-iteration shortcut
was applied to initial negotiation. The focused test proves the stopping
rule; routing that complete design is still required to establish convergence
or timing improvement.
