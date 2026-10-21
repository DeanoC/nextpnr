#!/usr/bin/env python3
"""Portable check of the sampled-load waveform reference, not timing signoff."""
import hashlib,json,sys,tarfile,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next(p for p in here.parents if (p/'mistral/tests/gpio-timing').is_dir())
sys.path.insert(0,str(repo/'mistral/tests/gpio-timing'))
from check_clock_waveform import check
manifest=json.loads((here/'manifest.json').read_text())
for name,digest in manifest['code_sha256'].items():
 assert hashlib.sha256((repo/name).read_bytes()).hexdigest()==digest,name
for name,digest in manifest['evidence_sha256'].items():
 assert hashlib.sha256((here/name).read_bytes()).hexdigest()==digest,name
assert manifest['new_pcxt_routes']==0 and manifest['fits']==3
assert manifest['remaining_route_seconds']==1627.843715112
assert not manifest['physical_signoff'] and not manifest['native_model_changed']
with tempfile.TemporaryDirectory() as tmp:
 with tarfile.open(here/'reference-fits.tar.gz') as tar:tar.extractall(tmp,filter='data')
 root=Path(tmp)
 for name,digest in json.loads((here/'files.json').read_text()).items():
  assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
 check(root,json.loads((here/'reference.json').read_text()))
print('PASS: frozen waveform evidence and code identities; no native model or physical acceptance change')
