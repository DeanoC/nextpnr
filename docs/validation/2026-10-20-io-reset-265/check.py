#!/usr/bin/env python3
"""Portable host-only IO coverage evidence. No routing or physical qualification."""
import gzip,importlib.util,json
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next((p for p in here.parents if (p/'mistral/routed_swap.py').exists()),Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr'))
spec=importlib.util.spec_from_file_location('guard',repo/'mistral/routed_swap.py');guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
s=json.loads(gzip.decompress((here/'pcxt-host-diagnostic.json.gz').read_bytes()))
a,b=s['original'],s['synthetic']
assert all(v['schema']==3 and not v['final_analogue'] and not v['complete'] for v in s.values())
assert len(a['unsupported'])==20 and len(b['unsupported'])==70
assert len(a['static_endpoints'])==1888 and a['static_endpoints']==b['static_endpoints']
assert sum(map(len,a['endpoints'].values()))==17147
assert sum(map(len,b['endpoints'].values()))==17163
assert a['clocks']==b['clocks'] and all(b['endpoints'][k]==v for k,v in a['endpoints'].items())
old,new=set(map(tuple,a['unsupported'])),set(map(tuple,b['unsupported']))
assert len(old-new)==16 and all(p=='DATAIN' for c,p in old-new)
assert len(old&new)==4 and all(p=='ACLR' for c,p in old&new)
assert len(new-old)==66 and sum(p=='I' for c,p in new-old)==48 and sum(p=='OE' for c,p in new-old)==18
assert not a['io_constraints'] and len(b['io_constraints'])==16
for constraint in b['io_constraints']:
 assert constraint['input'] and constraint['fall'] and constraint['clock']=='clocks[0]'
 assert constraint['min']==1 and constraint['max']==3
for endpoint in old-new:
 rows=next(v for k,v in b['endpoints'].items() if tuple(json.loads(k))==endpoint)
 assert len(rows)==1 and rows[0]['launch']==['clocks[0]',1] and rows[0]['capture']==['clocks[1]',0]
assert not guard.compare(a,b)[0]
f=json.loads(gzip.decompress((here/'io-fixtures.json.gz').read_bytes()))
assert guard.compare(f['baseline'],f['safe'])[0]
for name in ['missing','new_failure','hold_regression','clock_latency','min_changed','max_changed','phase','falling','external_reset','async_reset']:
 assert not guard.compare(f['baseline'],f[name])[0],name
for v in f.values():v.pop('io_constraints',None)
assert not guard.compare(f['baseline'],f['safe'])[0]
reset=json.loads((here/'reset-topology.json').read_text())
assert len(reset)==4 and all('async_reg' not in r['attrs'] and 'ASYNC_REG' not in r['attrs'] for r in reset)
first=[r for r in reset if r['ports']['DATAIN'][0]['driver'][2]=='MISTRAL_CONST']
second=[r for r in reset if r['ports']['DATAIN'][0]['driver'][2]=='MISTRAL_FF']
assert len(first)==len(second)==2
assert all(len(r['ports']['Q'][0]['users'])==1 for r in first)
assert sorted(len(r['ports']['Q'][0]['users']) for r in second)==[107,241]
assert len({r['ports']['ACLR'][0]['bit'] for r in reset})==1
for r in first:
 consumer=r['ports']['Q'][0]['users'][0]
 target=next(t for t in second if t['cell']==consumer[0])
 assert consumer[1]=='DATAIN' and r['ports']['CLK'][0]['bit']==target['ports']['CLK'][0]['bit']
m=json.loads((here/'manifest.json').read_text())
assert m['new_full_design_routes']==0 and m['total_charged_seconds']<=3600 and m['remaining_seconds']>=0
print('PASS: synthetic host-only constraints represent16 input paths; four ACLR and66 newly visible output/OE endpoints remain unsupported; input guard mutations reject')
