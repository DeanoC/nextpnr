#!/usr/bin/env python3
"""Check retained compiler comparison and its explicitly failing acceptance gates."""
import gzip
import json
from pathlib import Path
here = Path(__file__).resolve().parent
runs = {r['label']: r for r in map(json.loads, (here / 'runs.jsonl').read_text().splitlines())}
base, candidate = runs['baseline-expanded-route'], runs['candidate-expanded-route']
assert base['input_sha256'] == candidate['input_sha256']
assert base['source_commit'] != candidate['source_commit']
assert base['source_diff_sha256'] == candidate['source_diff_sha256']
def normalized(command):
    command = command.copy()
    command[0] = '<binary>'
    command[command.index('--json') + 1] = '<same-frozen-input>'
    return command
assert normalized(base['command']) == normalized(candidate['command'][:-4])
assert candidate['command'][-4:-1] == ['--critical-swap-budget', '64', '--critical-cohort-report']
for run in runs.values():
    assert run['status'] == 'failed' and run['returncode'] != 0
    assert run['timeout_seconds'] == 600
    assert '--timing-allow-fail' not in run['command']
    assert run['timing_summary']['final_analogue_model']
    assert run['fmax']['clocks[0]']['constraint'] == 100
    assert run['fmax']['clocks[0]']['achieved'] < 100
    assert all(c['hold_wns_ns'] >= 0 for c in run['timing_summary']['clocks'].values())
for clock, old in base['fmax'].items():
    now = candidate['fmax'][clock]
    assert now['constraint'] == old['constraint']
    assert now['achieved'] >= old['achieved']
assert candidate['timing_summary']['clocks']['clocks[0]']['setup_wns_ns'] < 0
for original, reload in [('baseline-run', 'baseline-reload'), ('candidate-final-route', 'candidate-final-reload')]:
    assert runs[original]['artifacts']['diagnostic.rbf'] == runs[reload]['artifacts']['diagnostic.rbf']
    assert runs[original]['fmax'] != runs[reload]['fmax']  # explicitly retained reload timing limitation
for original, expanded in [('baseline-run', 'baseline-expanded-route'), ('candidate-final-route', 'candidate-expanded-route')]:
    assert runs[original]['artifacts']['diagnostic.rbf'] == runs[expanded]['artifacts']['diagnostic.rbf']
    assert runs[original]['fmax'] == runs[expanded]['fmax']
legality = json.loads((here / 'legality.json').read_text())
assert legality['baseline']['returncode'] == legality['candidate']['returncode'] == 0
assert legality['baseline']['invalid'] == legality['candidate']['invalid']
assert len(legality['baseline']['invalid']) == 26
placement = json.loads((here / 'placement.json').read_text())
assert placement['cells'] == 10462
assert len(placement['differences']) == 4
assert all(d['path'][-1] == 'NEXTPNR_BEL' for d in placement['differences'])
endpoints = json.loads(gzip.decompress((here / 'setup-endpoints.json.gz').read_bytes()))
assert len(endpoints['rows']) == endpoints['coverage']['common'] == 2122
assert sum(r['before'] >= 0 > r['after'] for r in endpoints['rows']) == 15
manifest = json.loads((here / 'manifest.json').read_text())
assert abs(sum(r['charged_seconds'] for r in runs.values()) - manifest['charged_seconds']) < 1e-6
assert manifest['charged_seconds'] <= manifest['budget_seconds'] == 3600
print('PASS: paired inputs, four placement fields, improved clock summaries, identical reload RBFs; setup and 15 endpoint-regression gates explicitly FAIL')
