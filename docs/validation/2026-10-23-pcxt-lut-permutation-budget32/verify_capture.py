#!/usr/bin/env python3
"""Fresh safe extraction with complete dependency hashes and immutable proof replay."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tarfile

def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def checked_archive(path,expected=None):
 if expected:assert sha(path)==expected['archive_sha256'] and path.stat().st_size==expected['bytes']
 with tarfile.open(path,'r:gz') as t:
  members=t.getmembers();names=[m.name for m in members];assert len(names)==len(set(names))
  for m in members:assert m.isfile() and not Path(m.name).is_absolute() and '..' not in Path(m.name).parts and m.name==Path(m.name).as_posix()
  data=t.extractfile('manifest.json').read();manifest=json.loads(data);assert set(names)==set(manifest)|{'manifest.json'}
  if expected:assert len(names)==expected['members'] and hashlib.sha256(data).hexdigest()==expected['manifest_sha256']
  for member in members:
   if member.name=='manifest.json':continue
   v=manifest[member.name];stream=t.extractfile(member);assert member.size==v['size'] and hashlib.file_digest(stream,'sha256').hexdigest()==v['sha256'],member.name
 return manifest

def local_path(root,value):
 p=Path(value);assert not p.is_absolute() and '..' not in p.parts and p.as_posix()==value
 return root/p

def main():
 assert len(sys.argv)==5,'usage: verify_capture.py CAPTURE.tar.gz EMPTY_OUTPUT PRIVATE_NODE.tar.gz COMBINED.tar.gz'
 archive=Path(sys.argv[1]);out=Path(sys.argv[2]).resolve();manifest=checked_archive(archive)
 assert not out.exists() or not any(out.iterdir());out.mkdir(parents=True,exist_ok=True)
 with tarfile.open(archive) as t:t.extractall(out,filter='data')
 mode=json.loads((out/'archive-mode.json').read_text());deps={}
 for name,arg in zip(('node','combined'),sys.argv[3:]):
  spec=mode['dependencies'][name];deps[name]=checked_archive(Path(arg),spec)
  p=local_path(out,spec['manifest_path']);assert sha(p)==spec['manifest_sha256'] and json.loads(p.read_text())==deps[name]
 for v in mode['selected_dependency_members']:
  expected=deps[v['dependency']][v['member']];p=local_path(out,v['retained_path']);assert expected['sha256']==v['sha256']==sha(p) and expected['size']==p.stat().st_size
 # A failed mapping must never fall back to a still-existing original path.
 env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',TMPDIR=str(out.parent))
 p=subprocess.run([sys.executable,str(out/'replay.py')],cwd=out,env=env,capture_output=True,text=True)
 if p.returncode:print(p.stdout);print(p.stderr,file=sys.stderr);raise SystemExit(p.returncode)
 assert p.stdout.encode()==(out/'replay-proof.json').read_bytes(),'portable proof differs'
 review=out/'final-analysis/independent-review';before={str(p.relative_to(review)):sha(p) for p in review.iterdir() if p.is_file()}
 p=subprocess.run([sys.executable,str(out/'replay_review.py')],cwd=out,env=env,capture_output=True,text=True)
 if p.returncode:print(p.stdout);print(p.stderr,file=sys.stderr);raise SystemExit(p.returncode)
 assert before=={str(p.relative_to(review)):sha(p) for p in review.iterdir() if p.is_file()},'independent replay bytes differ'
 actual={str(p.relative_to(out)) for p in out.rglob('*') if p.is_file()};assert actual==set(manifest)|{'manifest.json'}
 for n,v in manifest.items():assert sha(out/n)==v['sha256'] and (out/n).stat().st_size==v['size'],n
 print(json.dumps({'passed':True,'capture_sha256':sha(archive),'capture_bytes':archive.stat().st_size,'members':len(manifest)+1,'manifest_sha256':sha(out/'manifest.json'),'complete_dependency_members':{n:len(m)+1 for n,m in deps.items()},'portable_proof':json.loads((out/'replay-proof.json').read_text()),'independent_review_sha256':sha(review/'review.json'),'scope':'Fresh extraction and offline graph/truth/timing/native-topology proof replay; no native or GPU execution'},indent=2,sort_keys=True))
if __name__=='__main__':main()
