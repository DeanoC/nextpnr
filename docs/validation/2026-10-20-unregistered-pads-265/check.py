#!/usr/bin/env python3
"""Portable audit of conditional pad references; never physical acceptance."""
import gzip,hashlib,importlib.util,json,sys,tarfile,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next(p for p in here.parents if (p/'mistral/routed_swap.py').exists())
sys.path.insert(0,str(repo/'mistral/tests/unregistered-pad'))
from reference_check import extract,compare_native
from hps_check import audit as audit_hps
read=lambda p:json.loads(gzip.decompress(p.read_bytes()) if p.suffix=='.gz' else p.read_text())
h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
m=read(here/'manifest.json')
assert m['new_pcxt_route_invocations']==0 and m['remaining_route_seconds']==1627.843715112 and not m['physical_signoff']
for name,digest in m['evidence_sha256'].items():assert h(here/name)==digest,name
identity=read(here/'source-identity.json')
assert identity['native_source_commit']=='03cf9cdce89e2d4fa667e1f760575fc4b09afc71'
assert '03cf9cdc' in identity['embedded_version']
for name,digest in identity['implementation_sha256'].items():assert h(repo/name)==digest,name
assert h(here/'CMakeCache.txt')==identity['build_cache_sha256']
assert 'GPU_ROUTER:STRING=OFF' in (here/'CMakeCache.txt').read_text()
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp)
 with tarfile.open(here/'reference-fits.tar.gz') as tar:tar.extractall(root,filter='data')
 result=extract(root);assert result==read(here/'reference.json')
 assert len(result['rows'])==2800 and len(result['rejected_input_boundaries'])==272
 assert len(result['models'])==16 and all(set(v)=={'data','oe','input'} for v in result['models'].values())
 assert compare_native(root,here/'native-config.json')
 assert audit_hps(root)==read(here/'hps-reference.json')
old=read(repo/'docs/validation/2026-10-20-interface-contracts-265/pcxt-host-diagnostic.json.gz')
new=read(here/'host-final.json.gz')
for phase,snapshot in old.items():
 assert new[phase]['schema']==7 and new[phase]['unregistered_contracts']=={}
 for key in snapshot:
  if key!='schema':assert snapshot[key]==new[phase][key],(phase,key)
a,b=new['reset'],new['unregistered']
assert len(b['endpoints'])==17246 and len(b['static_endpoints'])==1888
assert set(a['endpoints'])==set(b['endpoints'])
for key in ('clocks','io_constraints','primitive_clocks','registered_contracts','reset_contracts','forwarded_clocks','constant_outputs','static_endpoints','unsupported','unsupported_details'):
 assert a[key]==b[key],key
changes=[]
for key,rows in a['endpoints'].items():
 updated=b['endpoints'][key];assert len(rows)==len(updated)
 if rows==updated:continue
 changes.append(key)
 for before,after in zip(rows,updated):
  for f in before:
   if f not in ('max_path','setup'):assert before[f]==after[f],(key,f)
  delta=after['max_path']-before['max_path'];assert delta>0 and before['setup']-after['setup']==delta
assert len(changes)==48 and len(b['unregistered_contracts'])==48 and len(b['physical_obligations'])==52
assert b['unsupported']==[['scl','OE'],['sda','OE']] and b['complete'] is False and b['final_analogue'] is False
for contract in b['unregistered_contracts'].values():
 assert contract['physically_qualified'] is False
 assert contract['cell']['attrs']['NEXTPNR_INPUT_REG4_SEL']=='BYPASS'
 assert contract['cell']['attrs']['NEXTPNR_GPIO_TIMING_PROFILE']=='QUARTUS_17_0_2_DQ_UNREGISTERED'
 assert contract['rise'][0]==contract['fall'][0]==0
spec=importlib.util.spec_from_file_location('guard',repo/'mistral/routed_swap.py');guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
assert not guard.compare(b,b)[0]
forged=dict(b,complete=True,final_analogue=True,unsupported=[],unsupported_details={},physical_obligations=[],reset_contracts={},forwarded_clocks={})
assert not guard.compare(forged,forged)[0]
assert '[  PASSED  ] 95 tests.' in (here/'native-final.log').read_text()
assert 'Ran 24 tests' in (here/'driver-final.log').read_text() and (here/'driver-final.log').read_text().rstrip().endswith('OK')
assert '[  PASSED  ] 1 test.' in (here/'host-final.log').read_text()
assert (here/'reference-negative.log').read_text().count('rejected ')==6
for name in ('critical-swaps','routed-swap-guard','endpoint-coverage','io-reset','output-coverage','registered-pad-coverage','dynamic-ce-coverage','interface-contracts'):
 assert 'PASS:' in (here/'prior-final'/f'{name}.log').read_text()
cli=read(here/'cli-receipt.json');assert cli['status']=='rejected-hold' and cli['route_exit_code']==cli['replay_exit_code']==1
assert cli['replay_bit_exact'] and cli['replay_critical_paths_exact'] and cli['physical_signoff'] is False
with tempfile.TemporaryDirectory() as tmp:
 with tarfile.open(here/'cli-artifacts.tar.gz') as tar:tar.extractall(tmp,filter='data')
 root=Path(tmp)
 for name,digest in cli['sha256'].items():assert h(root/name)==digest,name
 assert (root/'top.rbf').read_bytes()==(root/'reloaded.rbf').read_bytes()
 assert (root/'report.json').read_bytes()==(root/'reloaded-report.json').read_bytes()

assert 'Hold/min time violation' in (here/'cli-route.log').read_text() and 'Hold/min time violation' in (here/'cli-reload.log').read_text()
attempts=read(here/'fit-attempts.json');assert len(attempts['attempts'])==7 and attempts['fit_limit']==8 and attempts['fit_timeout_seconds']==600
baseline=gzip.decompress((here/'native-baseline.rbf.gz').read_bytes())
profile=gzip.decompress((here/'native-profile-only.rbf.gz').read_bytes())
bypass=gzip.decompress((here/'native-bypass.rbf.gz').read_bytes())
assert baseline==profile and baseline!=bypass
assert h(here/'selector-delta.json')==m['evidence_sha256']['selector-delta.json']
delta=read(here/'selector-delta.json');assert delta['only_selector']=='INPUT_REG4_SEL' and delta['changed_lanes']==16
print('PASS: all16 DQ data/OE/input exact-boundary references; native selectors/models match; profile-only bytes unchanged; explicit BYPASS delta retained; prior17246 rows/1888 proofs preserved; HPS/high-Z/board qualification remains open; CLI hold failure preserved.')
