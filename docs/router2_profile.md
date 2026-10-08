# Router2 timeout diagnostics

`--router2-profile FILE` enables an optional router2 observer. It writes an
atomic JSON snapshot once per second and when router2 finishes. A killed router
leaves its last complete snapshot, with `completed: false`; a missing snapshot
establishes no progress information. The destination must be writable before
routing starts. Observer paths are per invocation: checkpoint reload clears
an inherited path and reapplies an explicitly supplied new path.

The `router2-profile-v1` schema reports total started/finished `route_net` calls,
net count, wall time since observer creation, and at most 20 active and 20 slow
nets. Names are bounded to 1024 UTF-8 bytes with a truncation flag. Active net
age starts with the first currently active call and ends when all of its active
calls finish. Slow-net totals sum finished call wall times, including repeated
visits; they are not exclusive CPU time or per-arc search statistics. The final
`completed` flag describes completion of the router2 phase, not timing acceptance
or later bitstream generation.

The observer cannot access routing costs, routing resources or the RNG. It adds
synchronization and I/O overhead when enabled, so performance comparisons must
record whether profiling was enabled. A long-running active net does not by
itself prove a deadlock or that the net is unroutable. Default routing has no
writer thread or snapshots.

Regression coverage:

- `common/route/tests/router2_profile.py`: concurrent visits, bounded results,
  UTF-8 truncation, unwritable output and a real killed-process snapshot.
- `mistral/tests/router2_profile.py`: native route equivalence and inherited
  observer-path clearing/replacement.

Both run in Mistral CI alongside the undriven RAM routing regression.
