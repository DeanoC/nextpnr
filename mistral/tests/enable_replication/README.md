# Optional LAB-aware enable replication

`--replicate-enables N` opts into a bounded post-HeAP pass, where N is 0..8.
The default is zero. The option is not inherited from loaded JSON metadata.
All original placements remain fixed. Partial-reconfiguration placement and
standalone SA are excluded.

An eligible ordinary ALUT2..6 drives only FF enable pins on one clock/edge
across multiple LABs. Placement-constrained, frozen, clustered, `keep`, and
`dont_touch` cells/nets are excluded. Actual MLAB memory occupancy protects a
LAB; ordinary LUT/FF use of an MLAB-capable tile remains eligible. Boundary,
clock, global, routed and architecture-pin nets cannot acquire new users.
Inputs must come from unconstrained ordinary LUTs or fabric FFs; already
folded constants preserve their pin state. Each upstream net may acquire at
most one extra input pin during the entire pass, and repeated signal inputs
on one candidate are excluded rather than undercounted.

Candidates rank by predicted failing setup slack, criticality, then stable
cell name. Search examines at most 32 original drivers and the Manhattan
radius-three neighborhoods of at most four critical sink LABs per driver.
Every input must have nonincreasing geometric delay. Every moved output must
improve by at least 250 ps; complete LAB groups move together, at least one
critical failing sink must improve, and an original group must remain. Ties
use stable tile/BEL enumeration. One replica per original driver is allowed.
Masks, folded input states and FF enable polarity are preserved.

A fresh timing analysis rejects new/worsened predicted hold violations.
This is a prediction guard: geometric delay ignores added electrical load,
and actual setup/hold acceptance still requires routing and final timing.
A rejected trial removes both its net and the automatically created net alias.

For diagnostic evidence set `NEXTPNR_MISTRAL_ENABLE_REPLICATION_DUMP` to an
existing-directory filename prefix. The pass writes `.before.json`,
`.after.json`, a `.pins.tsv` sidecar for each, and `.replicas.tsv`. The manifest
has one row per moved endpoint with columns `source`, `clone`, `bel`,
`min_gain_ps`, `sink`, `port`, `x`, `y`. Original netlists and clock initialization
stay in the same process; these snapshots are evidence, not restart files.

Run the portable production-policy test:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I mistral \
  mistral/tests/enable_replication/policy.cc -o /tmp/enable-policy
/tmp/enable-policy
```

Backend integration tests use real contexts, Mistral BELs, timing analysis and
the pass itself. They cover successful whole-LAB replication, constant and
inverted pins, ordinary logic in MLAB-capable tiles, unchanged original BELs,
disabled mode, protected/mixed consumers, frozen sinks, shared hard-block and
clock-input nets, alias-name collisions, and rejected-trial cleanup with a retry. The hold-new/worse predicate is exercised directly by
the policy test; the backend tests do not claim a routed hold guarantee.

```sh
git submodule update --init 3rdparty/googletest tests
cmake -S . -B build <normal Mistral options> -DBUILD_TESTS=ON
cmake --build build -j8
build/nextpnr-mistral-test --gtest_filter='EnableReplicationTest.*'
ctest --test-dir build --output-on-failure
```
