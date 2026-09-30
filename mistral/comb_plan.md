# Staged internal LUT-cut remapping

`--remap-comb-plan plan.json` runs one to eight explicit internal LUT-cut
remaps after fresh ordinary HeAP placement. It reuses the existing bounded
two/three-LUT composition, legality, endpoint setup, clock, hold and boundary
guards. All original cells and FF placements remain; an accepted step clones
the composed cut and redirects one ordinary LUT input. This experiment is
disabled unless explicitly requested on the CLI.

```json
{
  "steps": [
    {"report": "reports/first.json", "candidate": 0},
    {"report": "reports/second.json", "candidate": 0}
  ]
}
```

The schema requires exactly `steps` and exactly `report` and `candidate` in
each step. Duplicate keys, including escaped spellings, are rejected. Report
paths resolve relative to the plan file. Candidate indices must be integers
from zero through 2147483647. An index selects from the qualified candidates
in the current graph at that step; earlier accepted changes can alter later
qualification and indices.

All report files, syntax and request bounds are checked before placement.
Each stage validates relevant violated ENA/DATAIN paths against the current
cells, locations, continuous data edges, setup endpoint and register clock
attribution before probing mutations. A stale guide aborts the command.
A selected stage that fails qualification stops before routing, including
with `--force`. Each rejected probe restores its own graph, consumer slots
and pin states exactly. Earlier accepted stages remain only in the discarded
context if a later stage fails; no partial route or bitstream is published.
Placement failure also aborts before routing even with `--force`, so a plan
cannot be skipped by continuing with an unsuccessfully placed graph.

To list the final stage's candidates, give only that stage `candidate: -1`,
pass `--no-route`, and omit `--rbf`. Earlier selected stages still run; the
listing probes restore their own graph. A timing claim requires a subsequent
full route with the original constraints.

Comb plans run after the existing local remap or `--remap-plan` and before
capture/placed-reduction hooks. Every preceding local step must select a
candidate; local listing cannot precede a comb plan. Plans cannot be combined
with legacy `--remap-comb-critical`/`--remap-comb-candidate`, skipped packing or
placement, an already processed design, or FES slot/scaffold placement. Plan
options are CLI state and are never inherited from serialized JSON settings.
