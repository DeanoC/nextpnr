#!/usr/bin/env python3
"""Portable interface-contract audit. Deliberately no complete/physical acceptance."""
import gzip,hashlib,importlib.util,json,sys,tarfile,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next((p for p in here.parents if (p/'mistral/routed_swap.py').exists()),Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr'))
sys.path.insert(0,str(here))
from reference_check import audit
read=lambda p:json.loads(gzip.decompress(p.read_bytes()) if p.suffix=='.gz' else p.read_text())
spec=importlib.util.spec_from_file_location('guard',repo/'mistral/routed_swap.py');guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
a=read(repo/'docs/validation/2026-10-20-dynamic-ce-coverage-265/pcxt-host-diagnostic.json.gz')
b=read(here/'pcxt-host-diagnostic.json.gz')
fields=('endpoints','static_endpoints','clocks','io_constraints','output_contracts','registered_contracts','primitive_clocks')
for phase in a:
 for field in fields:assert a[phase][field]==b[phase][field],(phase,field)
 assert set(map(tuple,a[phase]['unsupported']))-set(map(tuple,b[phase]['unsupported']))=={('scl','I'),('sda','I')}
for snap in b.values():
 assert snap['schema']==6 and snap['complete'] is False and snap['final_analogue'] is False
 assert not guard.compare(snap,snap)[0]
 assert len(snap['static_endpoints'])==1888 and len(snap['constant_outputs'])==2
 assert set(snap['unsupported_details'])=={json.dumps(x) for x in snap['unsupported']}
 for key,proof in snap['constant_outputs'].items():
  assert json.loads(key)[1]=='I' and proof['value']==0
  assert proof['reason']=='constant-open-drain-data'
  assert proof['oe_endpoint'] in snap['unsupported']
final=b['reset'];base=b['registered'];forward=b['forwarded']
for field in ('endpoints','static_endpoints','clocks','io_constraints','registered_contracts','primitive_clocks'):
 assert final[field]==base[field]==forward[field],field
assert sum(map(len,final['endpoints'].values()))==17246
assert final['unsupported']==[['scl','OE'],['sda','OE']]
assert len(final['reset_contracts'])==2 and len(final['forwarded_clocks'])==1
assert len(final['physical_obligations'])==4
assert sorted(len(c['synchronous_sinks']) for c in final['reset_contracts'].values())==[259,554]
for c in final['reset_contracts'].values():
 assert c['status']=='structurally-validated' and c['physically_qualified'] is False
 assert c['reset_active']==0 and c['reset_value']==0 and len(c['raw_reset_fanout'])==4
 for stage in c['stages'].values():assert stage['attrs']['async_reg']=='true'
 required=c['synchronous_sinks']+[[stage,'DATAIN'] for stage in c['declaration']['stages'][1:]]
 for sink in required:
  rows=final['endpoints'][json.dumps(sink)]
  assert rows and all(r['setup_timed'] and r['hold_related'] and r['setup'] is not None and r['hold'] is not None for r in rows)
c=final['forwarded_clocks']['HDMI_TX_CLK']
assert c==forward['forwarded_clocks']['HDMI_TX_CLK']
assert c['configuration']=={'boundary':'GOUT.037.000.0000 / HMC.IOINTADDRDOUT.8','data_delay_selector':0,'oe_delay_selector':31,'data_inversion':False}
assert c['physically_qualified'] is False and c['pad_rise']==[0,6960] and c['pad_fall']==[0,6490]
assert c['minimum_high']==-226 and c['minimum_low']==244 # conservative pulse bound DOES NOT PASS
wave=final['clocks'][c['clock']]
assert c['rise']==[c['route'][i]+c['pad_rise'][i] for i in range(2)]
assert c['fall']==[wave[2+i]+c['route'][i]+c['pad_fall'][i] for i in range(2)]
# The sole added physical pad attributes are the separately declared profile/load.
for name,contract in final['output_contracts'].items():
 old=base['output_contracts'][name]
 if name!='HDMI_TX_CLK':assert contract==old
 else:
  clone=json.loads(json.dumps(contract))
  attrs=next(iter(clone['cells'].values()))['attrs']
  assert attrs.pop('NEXTPNR_GPIO_TIMING_PROFILE')=='QUARTUS_17_0_2_AG5_CLOCK'
  assert attrs.pop('BOARD_MODEL_FAR_C')=='30P'
  assert clone==old
intent=read(here/'pcxt-reset-intent-audit.json');overlay=read(here/'reset-overlay.json')
assert overlay['audit_sha256']==hashlib.sha256((here/'pcxt-reset-intent-audit.json').read_bytes()).hexdigest()
assert overlay['attrs']=={v['placed_cell']:{'async_reg':'true'} for v in intent['mapping']}
assert hashlib.sha256((here/'audited-fes_pcxt_top.sv').read_bytes()).hexdigest()==intent['rtl_sha256']
with tempfile.TemporaryDirectory() as temporary:
 root=Path(temporary)
 with tarfile.open(here/'reset-synthesis.tar.gz') as archive:archive.extractall(root,filter='data')
 for name in ('baseline','corrected','corrected_packed'):
  assert hashlib.sha256((root/(name+'.json')).read_bytes()).hexdigest()==intent['source_sha256'][name]
 baseline=read(root/'baseline.json');corrected=read(root/'corrected.json');packed=read(root/'corrected_packed.json')
 for cell in corrected['modules']['top']['cells'].values():cell['attributes'].pop('async_reg',None)
 assert baseline==corrected
 for mapping in intent['mapping']:
  assert packed['modules']['top']['cells'][mapping['intent_source_cell']]['attributes']['async_reg']=='true'
  assert mapping['placed_cell'] in overlay['attrs']
with tempfile.TemporaryDirectory() as temporary:
 with tarfile.open(here/'reference-fits.tar.gz') as archive:archive.extractall(temporary,filter='data')
 assert audit(Path(temporary))==read(here/'reference-receipt.json')
fixtures=read(here/'constant-fixtures.json.gz')
assert guard.compare(fixtures['baseline'],fixtures['safe'])[0]
assert not guard.compare(fixtures['hps'],fixtures['hps'])[0]
for name,key in (('reset-fixtures.json.gz','validated'),('forwarded-fixtures.json.gz','reference')):
 x=read(here/name)[key];assert not guard.compare(x,x)[0]
assert '[  PASSED  ] 91 tests.' in (here/'native-final.log').read_text()
assert 'Ran 24 tests' in (here/'driver-final.log').read_text() and (here/'driver-final.log').read_text().rstrip().endswith('OK')
assert 'PASS: 9 reference semantic corruptions rejected' in (here/'reference-negative.log').read_text()
manifest=read(here/'manifest.json');assert manifest['new_pcxt_route_invocations']==0 and manifest['remaining_route_seconds']==1627.843715112
assert manifest['physical_signoff'] is False
for name,digest in manifest['evidence_sha256'].items():assert hashlib.sha256((here/name).read_bytes()).hexdigest()==digest,name
print('PASS: unchanged17246 rows/1888 proofs; 2 I-only constants, 2 conditional reset chains, 1 reference clock; 2 HPS OE gaps; physical obligations and -226ps high-pulse lower bound remain nonpassing')
