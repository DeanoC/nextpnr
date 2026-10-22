# Deterministic replay supplement

The original 121-member capture is unchanged: SHA256
`18c8074b7fedba429cabec99d2a337a64b827506a12a0f7d2b06d1d43ae7b237`.
The initial root byte-comparison failure remains in
`capture/root-initial-readback-failure.json` and its logs. It exposed a renderer
ordering defect, without a difference in the captured timing comparison.

The archived `final-analysis/compare-routes.py` creates `margin_delta_ps`
dictionaries by iterating the set `{'setup','hold','min_path','max_path'}`
(lines 84 and 87). Python hash randomization changes that iteration order.
`json.dumps` then preserves the different dictionary insertion order.

`check.py` verifies every archived member and the manifest, extracts one private
working copy, and runs all three original analysis scripts with
`PYTHONHASHSEED=0`. It repeats the complete report comparison with seeds 1 and 2.
It also compares the exact root replay that triggered the initial failure.
Every comparison rejects duplicate keys, changed types, changed key inventories,
changed values and changed array order. The only permitted rendering differences
are the four margin keys in the complete endpoint rows and the repeated old
failure rows. All exact key-order permutations are retained in the small,
deterministically compressed `results/*-key-order-changes.json.gz` files.
Canonical JSON bytes are equal, with their lengths and hashes recorded.

All six checks pass. Both graph proof and third repair-decision replay reproduce
their original bytes. The timing comparison reproduces all parsed data under all
three explicit hash seeds. `results/result.json` records exact original/replayed
byte hashes, first differing byte offsets, canonical hashes, key permutations,
and log hashes. The original checker, archive and failed timing results were not
rewritten. The earlier byte-identical replay was valid for its process hash seed;
it did not establish deterministic rendering across fresh Python processes.

Reproduce from any location (the work directory must not exist):

```sh
python3 check.py --archive /path/to/capture.tar.gz \
  --work /path/to/new-private-extraction --output /path/to/new-results
```

The optional `--observed-root-replay` takes the initial root's regenerated
`route-comparison.json`. The retained result binds that observed file's exact
bytes. The `extracted/` working directory is disposable local replay state and
should not be copied into the durable supplement. Keep this README, checker and
`results/`, alongside the original archive and original root failure receipt.
This is offline evidence verification; no compiler, route or hardware ran.
