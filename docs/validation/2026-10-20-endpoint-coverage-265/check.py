#!/usr/bin/env python3
"""Check portable coverage/regression evidence, optionally exact retained files."""
import argparse,gzip,hashlib,importlib.util,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--artifact-root',type=Path);a=p.parse_args()
here=Path(__file__).resolve().parent
repo=here.parents[2]
if not (repo/'mistral/routed_swap.py').exists():repo=Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr')
spec=importlib.util.spec_from_file_location('guard',repo/'mistral/routed_swap.py');guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
prior=repo/'docs/validation/2026-10-20-routed-swap-guard-265'
prior_manifest=json.loads((prior/'manifest.json').read_text())
m=json.loads((here/'manifest.json').read_text());summary=json.loads((here/'summary.json').read_text());snapshots=[]
for trial in ('baseline','candidate'):
 raw=gzip.decompress((here/(trial+'-endpoints.json.gz')).read_bytes());assert hashlib.sha256(raw).hexdigest()==m['artifacts'][trial]['endpoints.json'];s=json.loads(raw);snapshots.append(s)
 old=json.loads(gzip.decompress((prior/(trial+'-endpoints.json.gz')).read_bytes()))
 assert old['endpoints']==s['endpoints'] and old['clocks']==s['clocks']
 assert {tuple(v) for v in old['unsupported']}==({tuple(v) for v in s['unsupported']} | {tuple(json.loads(v)) for v in s['static_endpoints']})
 assert m['artifacts'][trial]['diagnostic.rbf']==prior_manifest['artifacts'][trial]['diagnostic.rbf']
 assert s['schema']==2 and s['final_analogue'] and not s['complete']
 assert len(s['static_endpoints'])==1888 and len(s['unsupported'])==20
 assert sum(port=='ACLR' for _,port in s['unsupported'])==4
 assert sum(port=='DATAIN' for _,port in s['unsupported'])==16
 assert len(s['endpoints'])==summary[trial]['dynamic_endpoints']
 assert sum(map(len,s['endpoints'].values()))==17147
 assert not(set(s['static_endpoints']) & set(s['endpoints']))
 for proof in s['static_endpoints'].values():
  assert proof['reason']=='constant-no-launch-transition' and proof['value'] in (0,1)
  assert proof['capture'] and proof['chain'][-1]['type']=='MISTRAL_CONST'
  assert all(node['type']=='MISTRAL_BUF' for node in proof['chain'][:-1])
 for field in ('previous_rbf_equal','previous_dynamic_rows_equal','previous_clocks_equal','previous_fmax_equal','unsupported_accounted'):assert summary[trial][field],field
 if a.artifact_root:
  for f,h in m['artifacts'][trial].items():assert guard.digest(a.artifact_root/trial/f)==h,(trial,f)
assert snapshots[0]['static_endpoints']==snapshots[1]['static_endpoints']
assert guard.compare(*snapshots)==(False,'coverage failure: incomplete native endpoint coverage')
def rows(s):return {(e,tuple(r['launch']),tuple(r['capture'])):r for e,v in s['endpoints'].items() for r in v}
x,y=map(rows,snapshots);assert x.keys()==y.keys()
new_failures=[k for k,v in x.items() if v['setup_timed'] and v['setup']>=0>y[k]['setup']]
assert len(new_failures)==15==summary['newly_failing_setup']
assert sum(v['setup_timed'] and y[k]['setup']<v['setup'] for k,v in x.items())==7135
assert sum(v['hold_related'] and y[k]['hold']<v['hold'] for k,v in x.items())==6181
# The known unsafe pair remains unsafe independently of the incomplete-coverage
# gate. Synthetic one-pair native fixtures isolate each genuine newly failing row;
# they are a regression test, never a full-design acceptance decision.
for k in new_failures:
 e,_,_=k;pair=[]
 for s,r in ((snapshots[0],x[k]),(snapshots[1],y[k])):
  pair.append(dict(schema=6,constant_outputs={},reset_contracts={},physical_obligations=[],registered_contracts={},primitive_clocks={},output_contracts={},io_constraints=[],final_analogue=True,complete=True,unsupported=[],static_endpoints={},clocks=s['clocks'],endpoints={e:[r]}))
 assert guard.compare(*pair)[0] is False
 assert 'setup regression' in guard.compare(*pair)[1]
assert summary['decision']['selected']=='baseline' and all(summary['exact_retention'].values())
assert m['total_charged_seconds']<=3600 and m['remaining_seconds']>=0
if a.artifact_root:
 for f in guard.ARTIFACTS:assert (a.artifact_root/'retained'/f).read_bytes()==(a.artifact_root/'baseline'/f).read_bytes(),f
print('PASS: 1888 static proofs, 16 unresolved IO and four reset paths, 15 genuine new setup failures; exact baseline retained')
