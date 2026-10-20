#!/usr/bin/env python3
"""Portable dynamic-CE coverage receipt: diagnostic, never physical signoff."""
import gzip,hashlib,json,sys,tarfile,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next((p for p in here.parents if (p/'mistral/tests').is_dir()),Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr'))
sys.path.insert(0,str(here))
from reference_check import audit

def read(p):
    return json.loads(gzip.decompress(p.read_bytes()) if p.suffix=='.gz' else p.read_text())
old=read(repo/'docs/validation/2026-10-20-registered-pad-coverage-265/pcxt-host-diagnostic.json.gz')
new=read(here/'pcxt-host-diagnostic.json.gz')
a=old['registered'];b=new['registered']
for phase in ('original','synthetic'):
    assert old[phase]==new[phase],phase
assert sum(map(len,a['endpoints'].values()))==17233
assert sum(map(len,b['endpoints'].values()))==17246
assert b['schema']==5 and b['complete'] is False and b['final_analogue'] is False
assert b['clocks']==a['clocks'] and b['static_endpoints']==a['static_endpoints']
assert len(b['static_endpoints'])==1888
for field in ('endpoints','registered_contracts','primitive_clocks'):
    assert all(b[field].get(k)==v for k,v in a[field].items()),field
added=set(b['endpoints'])-set(a['endpoints']);assert len(added)==13
resolved=set(map(tuple,a['unsupported']))-set(map(tuple,b['unsupported']))
assert len(resolved)==13 and all(port=='PAD' for cell,port in resolved)
assert len(b['unsupported'])==9
assert set(map(tuple,b['unsupported']))==set(map(tuple,a['unsupported']))-resolved
assert len(b['registered_contracts'])==21 and len(b['primitive_clocks'])==21
changed=[]
for key,contract in b['registered_contracts'].items():
    if key in a['registered_contracts']:continue
    cell,pad,direction=json.loads(key)
    assert (cell,pad) in resolved and direction is False
    assert contract['type']=='MISTRAL_SDROUT'
    assert set(contract['channels'])=={'data'}
    channel=contract['channels']['data']
    endpoint=next(k for k in added if json.loads(k)==channel['endpoint'])
    row=b['endpoints'][endpoint]
    assert len(row)==1 and row[0]['launch']==['clocks[0]',0]
    clk=next(v for k,v in b['primitive_clocks'].items() if json.loads(k)==[cell,'CLK'])
    assert clk['limits']==[1540,790,770] and all(v>=0 for v in clk['margins'])
    # Old native data and CE capture paths are separately required and exact.
    for port in ('I','CEOUT'):
        prior=next(k for k in a['endpoints'] if json.loads(k)==[cell,port])
        assert b['endpoints'][prior]==a['endpoints'][prior]
    changed.append({'cell':cell,'pin':contract['attrs']['LOC'],'endpoint':channel['endpoint'],'rows':row})
assert read(here/'changed-identities.json')==sorted(changed,key=lambda x:x['cell'])
assert read(here/'remaining-endpoints.json')==b['unsupported']
with tempfile.TemporaryDirectory() as temporary:
    with tarfile.open(here/'reference-fits.tar.gz') as archive:
        archive.extractall(temporary,filter='data')
    assert audit(Path(temporary))==read(here/'reference-receipt.json')
for fixture in ('registered-fixtures.json.gz','forwarder-fixtures.json.gz'):
    assert read(here/fixture)==read(repo/'docs/validation/2026-10-20-registered-pad-coverage-265'/fixture)
assert '[  PASSED  ] 88 tests.' in (here/'native-final.log').read_text()
assert 'Ran 21 tests' in (here/'driver-final.log').read_text()
assert (here/'driver-final.log').read_text().rstrip().endswith('OK')
assert all(x['rows'][0]['setup']<0 for x in changed)
manifest=read(here/'manifest.json')
assert manifest['new_pcxt_route_invocations']==0 and manifest['bounded_primitive_fits']==2
assert manifest['remaining_route_seconds']==1627.843715112
for name,digest in manifest['evidence_sha256'].items():
    assert hashlib.sha256((here/name).read_bytes()).hexdigest()==digest,name
print('PASS: 13 additional pads, unchanged 17233 rows/1888 proofs and prior clocks, 9 open gaps; reference fits remain within unchanged bounds')
