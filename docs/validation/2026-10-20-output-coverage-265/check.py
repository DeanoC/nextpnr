#!/usr/bin/env python3
"""Portable output inventory evidence; never authorizes routing or physical signoff."""
import gzip,hashlib,importlib.util,json
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next((p for p in here.parents if (p/'mistral/routed_swap.py').exists()),Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr'))
spec=importlib.util.spec_from_file_location('guard',repo/'mistral/routed_swap.py');guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
s=json.loads(gzip.decompress((here/'pcxt-host-diagnostic.json.gz').read_bytes()))
a,b=s['original'],s['synthetic']
assert all(v['schema']==4 and not v['final_analogue'] and not v['complete'] for v in s.values())
assert len(a['unsupported'])==107 and len(b['unsupported'])==30
assert sum(map(len,a['endpoints'].values()))==17147 and sum(map(len,b['endpoints'].values()))==17224
assert a['clocks']==b['clocks'] and a['static_endpoints']==b['static_endpoints'] and len(a['static_endpoints'])==1888
assert all(b['endpoints'].get(k)==v for k,v in a['endpoints'].items())
assert not a['io_constraints'] and a['output_contracts']=={}
assert len(b['io_constraints'])==64 and len(b['output_contracts'])==48
old,new=set(map(tuple,a['unsupported'])),set(map(tuple,b['unsupported']))
assert new<old and len(old-new)==77
assert len([1 for c,p in new if p=='PAD'])==21
assert len([1 for c,p in new if p=='ACLR'])==4
assert not guard.compare(a,b)[0]
summary=json.loads((here/'host-summary.json').read_text())
assert all(summary[k] for k in ('old_original_rows_identical','old_input_rows_identical','old_static_proofs_identical','old_clocks_identical'))
assert summary['resolved_output_classes']=={'sdram-data':16,'sdram-oe':16,'sdram-mask':2,'hdmi-data-control':27}
assert summary['remaining_classes']=={'registered-sdram-pad':21,'reset-aclr':4,'hdmi-clock-forwarder':1,'hps-i2c-constant-data':2,'hps-i2c-enable-unmodeled-source':2}
physical=json.loads((here/'physical-output-accounting.json').read_text())
assert len(physical)==69 and all(r['dynamic_endpoints'] or r['unsupported'] for r in physical)
f=json.loads(gzip.decompress((here/'output-fixtures.json.gz').read_bytes()))
# Isolated native fixture envelopes only; historical physical inventories remain schema 4.
for v in f.values():v.update(schema=6,constant_outputs={},reset_contracts={},physical_obligations=[],registered_contracts={},primitive_clocks={})
assert guard.compare(f['baseline'],f['safe'])[0]
for n in ('new_failure','hold_regression','clock_latency','missing','min_changed','max_changed','phase','falling_capture','falling_launch','constant'):
 assert not guard.compare(f['baseline'],f[n])[0],n
assert len(f['baseline']['endpoints'])==3 and f['constant']['unsupported']==[['output_pad','I']]
for v in f.values(): v.pop('output_contracts',None)
assert not guard.compare(f['baseline'],f['safe'])[0]
m=json.loads((here/'manifest.json').read_text())
assert m['new_full_design_routes']==0 and m['total_charged_seconds']<3600 and m['new_empty_cpu_routes']==2
for name,digest in m['evidence_sha256'].items():assert hashlib.sha256((here/name).read_bytes()).hexdigest()==digest,name
print('PASS: 61 output/OE +16 input rows; 30 explicit unsupported; schema4 contracts; exact historical rows/proofs; no physical qualification')
