#!/usr/bin/env python3
"""Portable conditional registered-pad evidence; never promotes physical coverage."""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
here = Path(__file__).resolve().parent
repo = next((p for p in here.parents if (p / 'mistral/routed_swap.py').exists()), Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr'))
spec = importlib.util.spec_from_file_location('guard', repo / 'mistral/routed_swap.py')
guard = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard)
def compressed(path):
    return json.loads(gzip.decompress(path.read_bytes()))
s = compressed(here / 'pcxt-host-diagnostic.json.gz')
a, b, c = (s[k] for k in ('original', 'synthetic', 'registered'))
assert all(v['schema'] == 5 and not v['complete'] and not v['final_analogue'] for v in s.values())
assert [sum(map(len, v['endpoints'].values())) for v in (a,b,c)] == [17147,17224,17233]
assert [len(v['unsupported']) for v in (a,b,c)] == [107,30,22]
assert all(v['clocks'] == a['clocks'] and v['static_endpoints'] == a['static_endpoints'] for v in s.values())
assert len(a['static_endpoints']) == 1888
assert all(c['endpoints'].get(k) == v for k,v in b['endpoints'].items())
old = compressed(repo / 'docs/validation/2026-10-20-output-coverage-265/pcxt-host-diagnostic.json.gz')
for name in old:
    assert all(s[name][field] == old[name][field] for field in ('endpoints','clocks','static_endpoints','io_constraints','output_contracts','unsupported'))
assert not guard.compare(c,c)[0]
resolved = set(map(tuple,b['unsupported'])) - set(map(tuple,c['unsupported']))
assert len(resolved) == 8 and all(pin == 'PAD' for _,pin in resolved)
assert len(c['registered_contracts']) == 8 and len(c['primitive_clocks']) == 8
new = set(c['endpoints']) - set(b['endpoints'])
assert len(new) == 9
covered = set()
for key, contract in c['registered_contracts'].items():
    cell,pad,direction = json.loads(key)
    assert (cell,pad) in resolved and direction is False
    assert contract['attrs']['NEXTPNR_GPIO_TIMING_PROFILE'] == 'QUARTUS_17_0_2_RAMTEST'
    assert contract['attrs']['BOARD_MODEL_FAR_C'] == '30P'
    assert contract['bel'] and contract['net'] and contract['port']
    assert set(contract['channels']) == ({'rise','fall'} if cell.startswith('SDRAM_CLK_') else {'data'})
    for channel in contract['channels'].values():
        endpoint = next(k for k in new if json.loads(k) == channel['endpoint'])
        assert endpoint not in covered; covered.add(endpoint)
        row = c['endpoints'][endpoint][0]
        clock_key = next(k for k in c['primitive_clocks'] if json.loads(k) == [cell,channel['clock_port']])
        clock = c['primitive_clocks'][clock_key]
        assert clock['limits'] == [1540,790,770]
        assert row['launch'] == [clock['clock'],channel['edge']]
        assert all(v >= 0 for v in clock['margins'])
assert covered == new
remaining = json.loads((here/'remaining-endpoints.json').read_text())
assert {(x['cell'],x['port']) for x in remaining} == set(map(tuple,c['unsupported']))
excluded = json.loads((here/'excluded-registered-pads.json').read_text())
assert len(excluded) == 13 and all(x['CEOUT'] for x in excluded)
assert {x['cell'] for x in excluded} == {x['cell'] for x in remaining if x['port']=='PAD'}
physical = json.loads((here/'physical-output-accounting.json').read_text())
assert len(physical) == 69 and all(x['dynamic_endpoints'] or x['unsupported'] for x in physical)
for x in physical:
    if x['type'] in ('MISTRAL_SDROUT','MISTRAL_DDROUT'):
        assert x['registered_contracts'] or 'PAD' in x['unsupported']
f = compressed(here/'registered-fixtures.json.gz')
assert guard.compare(f['baseline'],f['safe'])[0]
assert len(f['baseline']['registered_contracts']) == 2
assert {tuple(sorted(x['channels'])) for x in f['baseline']['registered_contracts'].values()} == {('high','low'),('data','oe')}
for name in ('clock_regression','setup_failure','hold_regression'):
    assert not guard.compare(f['baseline'],f[name])[0]
assert 'primitive clock margin regression' in guard.compare(f['baseline'],f['clock_regression'])[1]
assert all(v > 0 for c0 in f['clock_regression']['primitive_clocks'].values() for v in c0['margins'])
forward = compressed(here/'forwarder-fixtures.json.gz')
for polarity, snapshot in forward.items():
    assert snapshot['final_analogue'] is False
    contract = next(iter(snapshot['registered_contracts'].values()))
    assert set(contract['channels']) == {'rise','fall'}
    for name, channel in contract['channels'].items():
        assert channel['edge'] == (0 if (name=='rise') == (polarity=='normal') else 1)
        key = next(k for k in snapshot['endpoints'] if json.loads(k)==channel['endpoint'])
        row = snapshot['endpoints'][key][0]
        assert row['setup'] == (3600 if channel['edge']==0 else -1380)
        assert row['hold'] == (0 if channel['edge']==0 else 5000)
manifest = json.loads((here/'manifest.json').read_text())
assert manifest['new_route_invocations'] == 0 and manifest['remaining_route_seconds'] == 1627.843715112
for name,digest in manifest['evidence_sha256'].items():
    assert hashlib.sha256((here/name).read_bytes()).hexdigest() == digest, name
print('PASS: 8 qualified conditional pads / 9 channels; 13 dynamic-CE pads and 9 other gaps remain; all prior rows/proofs/clocks identical; native waveform retention; no physical signoff')
