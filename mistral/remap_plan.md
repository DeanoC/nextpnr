# Staged local remapping

`--remap-plan plan.json` runs one to sixteen explicit local LUT remaps after
fresh full-design HeAP placement. Each step uses the existing composition,
whole-LAB enable-group selection, legality and timing guards. This opt-in
experiment is disabled unless the CLI requests it.

`--remap-post-plan post.json` uses the same schema for additional local remaps
after internal-cut remapping, capture and placed reductions, and control
decomposition, before joint LUT placement and LUT driver copying. Early and post plans
together may contain at most sixteen steps. Either plan can be used alone.
Each plan resolves its reports relative to its own file. A post plan is also
explicit CLI state and is disabled by default.

```json
{
  "steps": [
    {
      "report": "reports/first.json",
      "candidate": 0,
      "groups": 4,
      "optimize_pins": false,
      "preserve_ff_placement": true
    },
    {
      "report": "reports/second.json",
      "candidate": 0,
      "groups": 1,
      "optimize_pins": false,
      "preserve_ff_placement": true
    }
  ]
}
```

The schema requires exactly these fields and rejects duplicate keys. Report
paths resolve relative to the plan file. `groups` is an integer from one to
eight. A candidate index selects from the qualified candidates in the graph
at that step, with that step's options. Earlier accepted remaps can change
later qualification and indices. `preserve_ff_placement` restricts trials
to the original FF locations; `optimize_pins` enables the existing LUT input
order search. The legacy single-remap CLI also accepts `--remap-preserve-ffs`.

All report files and request bounds are checked before placement. Immediately
before each step, relevant violated ENA paths must match the current cell
locations, continuous data edges, setup endpoint and register clock endpoints.
Reports from a different mapping or placement are rejected. A failed selected
step aborts before routing, including with `--force`. A rejected probe restores
its own graph exactly. Accepted earlier steps remain in the discarded failing
context; the command does not publish a partial route or bitstream.

Both plans' syntax, bounds and listing requests are checked before any early
plan step changes the graph. Live report edges are checked only at the step
that consumes them. A post report must describe the graph after the preceding
transformations; changing a report's labels does not make stale edges valid.
Post steps use the same composition, placement and timing guards as early
steps, and restore the caller's pin-search and FF-placement options on exit.

To enumerate candidates for the last step, use `candidate: -1` there, pass
`--no-route`, and omit `--rbf`. Earlier selected steps still run, and listing
probes restore their graph. This provides candidate indices in the actual
staged context without routing. Timing claims require a subsequent complete
route with normal constraints.

Listing must be final across the complete transformation sequence. An early,
internal-cut, placed-reduction or decomposition listing cannot precede a post
plan. A post listing cannot precede a joint LUT placement or LUT-driver-copy
request, including a listing. The same final-listing rule applies to a
[`--remap-lut-pair-copy-plan`](lut_pair_copy_plan.md), which occupies the
LUT-pair stage and can retain up to two copies for disjoint enable cohorts.
Selected post steps may precede selected or listing
pair and driver requests. Neither a selected candidate nor a listing proves routed timing,
exported physical truth or hardware behavior.

Early plans cannot be combined with legacy local-remap options. Both plan
kinds reject skipped packing or placement, an already processed design, or
FES slot/scaffold placement. Plan
options are CLI state and are never enabled by serialized JSON settings. Plans
requested with `--remap-plan` run before the optional internal-cut remap and
capture/placed-reduction hooks.
Selected local stages may precede an explicit
[`--remap-comb-plan`](comb_plan.md) of internal cuts. Local listing cannot
precede an internal-cut plan because listing must be final.
