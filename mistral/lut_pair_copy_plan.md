# Staged composed LUT copies

`--remap-lut-pair-copy-plan plan.json` requests one or two isolated composed
LUT copies after the post-local plan and before LUT driver copying. It uses
the same Boolean, whole-LAB enable-cohort, placement and timing checks as
`--remap-lut-pair-compose-copy`. The option is disabled by default and is
never activated by saved JSON settings.

```json
{
  "steps": [
    {"report": "reports/first.json", "candidate": 0},
    {"report": "reports/after-first.json", "candidate": 0}
  ]
}
```

The schema accepts only `steps`, and each step accepts exactly `report` and
`candidate`. Duplicate keys, missing reports, nonintegral indices and more
than two steps are rejected. Paths resolve relative to the plan file. All
reports' syntax is checked before placement; the current graph, locations,
clock attribution and continuous report edges are checked at each step.
The second report must describe the graph after the first selected copy.
Candidate indices belong to the qualified search at that particular step.

An accepted step retains both original LUTs and their existing consumers.
It creates one equivalent LUT with a private output for the complete safe
same-LAB FF-enable cohort. Later steps cannot retarget any FF enable changed
by an earlier step. Other disjoint cohorts can use their own copies. Each
probe snapshots the current graph, including copies already accepted by the
plan, and restores it if qualification fails.

A selected step that does not qualify stops the command before routing,
including with `--force`. Earlier accepted steps remain only in the failing
context; no partial route or bitstream is published. To list candidates for
the final step, use `candidate: -1`, pass `--no-route` and omit `--rbf`.
Listing probes preserve the earlier accepted copies. Every earlier stage
must select a candidate; a final listing cannot precede a driver-copy request.

The plan requires fresh packing and ordinary full-design HeAP placement.
It conflicts with the three legacy LUT-pair request options, rejects skipped
or processed placement and FES slot/scaffold flows, and preserves the existing
upstream HeAP policy. A qualified placement candidate still requires a full
route, final setup/hold checks and independent equivalence evidence before
claiming a timing gain. No combined routed gain is established by this option.
