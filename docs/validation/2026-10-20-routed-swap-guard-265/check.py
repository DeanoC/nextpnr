#!/usr/bin/env python3
"""Check the portable full-route guard evidence; optionally hash retained artifacts."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--artifact-root', type=Path)
a = p.parse_args()
here = Path(__file__).resolve().parent
repo = here.parents[2]
spec = importlib.util.spec_from_file_location('guard', repo / 'mistral/routed_swap.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
manifest = json.loads((here / 'manifest.json').read_text())
comparison = json.loads((here / 'comparison.json').read_text())
snapshots = []
for name in ('baseline', 'candidate'):
    raw = gzip.decompress((here / (name + '-endpoints.json.gz')).read_bytes())
    assert hashlib.sha256(raw).hexdigest() == manifest['artifacts'][name]['endpoints.json']
    snapshots.append(json.loads(raw))
accepted, reason = guard.compare(*snapshots)
assert not accepted
assert reason == comparison['decision']['reason']
assert comparison['decision']['selected'] == 'baseline'
assert all(comparison['exact_retention'].values())
assert comparison['prior_baseline_rbf_equal'] and comparison['prior_candidate_rbf_equal']
assert comparison['prior_baseline_clock_summary_equal'] and comparison['prior_candidate_clock_summary_equal']
assert comparison['newly_failing_setup'] > 0
assert manifest['total_charged_seconds'] <= 3600
if a.artifact_root:
    for trial, artifacts in manifest['artifacts'].items():
        for name, digest in artifacts.items():
            assert guard.digest(a.artifact_root / trial / name) == digest, (trial, name)
    for name in guard.ARTIFACTS:
        assert (a.artifact_root / 'retained' / name).read_bytes() == (a.artifact_root / 'baseline' / name).read_bytes()
print('PASS: unsafe full-route candidate rejected; baseline artifacts retained exactly; no timing closure claimed')
