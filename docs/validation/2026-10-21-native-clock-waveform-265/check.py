#!/usr/bin/env python3
"""Verify native waveform diagnostics and unchanged host timing inventory."""
import copy
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
here = Path(__file__).resolve().parent
repo = next(p for p in here.parents if (p/'mistral/routed_swap.py').is_file())
manifest = json.loads((here/'manifest.json').read_text())
for name, digest in manifest['code_sha256'].items():
    assert hashlib.sha256((repo/name).read_bytes()).hexdigest() == digest, name
for name, digest in manifest['evidence_sha256'].items():
    assert hashlib.sha256((here/name).read_bytes()).hexdigest() == digest, name
sys.path.insert(0, str(repo/'mistral/tests/gpio-timing'))
from clock_waveform import evaluate
reference = json.loads((repo/'docs/validation/2026-10-21-clock-waveform-265/reference.json').read_text())

def load(name):
    return json.loads(gzip.decompress((here/name).read_bytes()))

def check(snapshot, port):
    assert snapshot['complete'] is False
    c = snapshot['forwarded_clocks'][port]
    assert c['physically_qualified'] is False
    assert c['model'] == 'QUARTUS_17_0_2_AD20_INVERTED_POINT_LOAD'
    spread = c['route'][1] - c['route'][0]
    assert spread >= 0
    expected = evaluate(reference, c['declaration']['load_pf'], c['source_high'][0]-spread,
                        c['source_low'][0]-spread, c['declaration']['uncertainty_ps'])
    assert c['minimum_high'] == expected['high_min_ps']
    assert c['minimum_low'] == expected['low_min_ps']
    assert any('source/buffer waveform distortion' in o for o in snapshot['physical_obligations'])

native = load('native-fixture.json.gz')
check(native, 'dout')
old, new = load('host-before.json.gz'), load('host-after.json.gz')
check(new, 'SDRAM_CLK')
assert old['final_analogue'] is False and new['final_analogue'] is False
assert old.keys() == new.keys()
for key in old.keys() - {'forwarded_clocks', 'physical_obligations'}:
    assert old[key] == new[key], key
assert set(new['forwarded_clocks']) == set(old['forwarded_clocks']) | {'SDRAM_CLK'}
for key in old['forwarded_clocks']:
    assert old['forwarded_clocks'][key] == new['forwarded_clocks'][key]
assert set(old['physical_obligations']) < set(new['physical_obligations'])
# Exercise the actual guard directly, after removing unrelated blockers.
spec = importlib.util.spec_from_file_location('guard', repo/'mistral/routed_swap.py')
guard = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard)
for qualified in (False, True):
    forged = copy.deepcopy(new)
    forged.update(complete=True, final_analogue=True, unsupported=[], unsupported_details={},
                  physical_obligations=[], reset_contracts={})
    forged['forwarded_clocks'] = {'SDRAM_CLK': copy.deepcopy(new['forwarded_clocks']['SDRAM_CLK'])}
    forged['forwarded_clocks']['SDRAM_CLK']['physically_qualified'] = qualified
    ok, reason = guard.compare(forged, forged)
    assert not ok and 'forwarded clock' in reason, reason
assert manifest['new_pcxt_routes'] == 0 and manifest['hardware_used'] is False
print('PASS: native and PC/XT diagnostic arithmetic; all old host rows unchanged; forged qualification rejects')
