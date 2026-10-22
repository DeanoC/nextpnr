#!/usr/bin/env python3
"""Safe fresh extraction, exact complete dependency inventories, then offline host proof replay."""
from pathlib import Path
import hashlib,json,os,subprocess,sys,tarfile

def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def checked_archive(path,expected=None):
 if expected:
  assert sha(path)==expected['archive_sha256'] and path.stat().st_size==expected['bytes']
 with tarfile.open(path,'r:gz') as t:
  members=t.getmembers();names=[m.name for m in members];assert len(names)==len(set(names))
  for m in members:assert m.isfile() and not Path(m.name).is_absolute() and '..' not in Path(m.name).parts and m.name==Path(m.name).as_posix()
  data=t.extractfile('manifest.json').read();manifest=json.loads(data);assert set(names)==set(manifest)|{'manifest.json'}
  if expected:assert len(names)==expected['members'] and hashlib.sha256(data).hexdigest()==expected['manifest_sha256']
  for member in members:
   if member.name=='manifest.json':continue
   b=t.extractfile(member).read();v=manifest[member.name];assert len(b)==member.size==v['size'] and hashlib.sha256(b).hexdigest()==v['sha256'],member.name
 return manifest

def main():
 assert len(sys.argv)==6,'usage: verify_capture.py CAPTURE.tar.gz EMPTY_OUTPUT PRIOR.tar.gz FRONTIER.tar.gz CPU.tar.gz'
 archive=Path(sys.argv[1]);out=Path(sys.argv[2]);manifest=checked_archive(archive)
 assert not out.exists() or not any(out.iterdir());out.mkdir(parents=True,exist_ok=True)
 with tarfile.open(archive) as t:t.extractall(out,filter='data')
 mode=json.loads((out/'archive-mode.json').read_text());deps={}
 for name,arg in zip(('prior','frontier','cpu'),sys.argv[3:]):
  dep=mode['dependencies'][name];m=checked_archive(Path(arg),dep)
  assert hashlib.sha256((out/dep['manifest_path']).read_bytes()).hexdigest()==dep['manifest_sha256'] and json.loads((out/dep['manifest_path']).read_text())==m
  deps[name]=m
 for item in mode['selected_dependency_members']:
  expected=deps[item['dependency']][item['member']];assert item['sha256']==expected['sha256']==sha(out/item['retained_path']) and (out/item['retained_path']).stat().st_size==expected['size']
 for item in mode['raw_input_bindings']:assert deps[item['dependency']][item['member']]['sha256']==item['sha256']
 env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1');p=subprocess.run([sys.executable,str(out/'verify_host.py')],cwd=out,env=env,capture_output=True,text=True)
 if p.returncode:print(p.stdout);print(p.stderr,file=sys.stderr);raise SystemExit(p.returncode)
 assert p.stdout.encode()==(out/'host-proof.json').read_bytes(),'offline host proof differs'
 host=json.loads(p.stdout)
 actual={str(p.relative_to(out)) for p in out.rglob('*') if p.is_file()};assert actual==set(manifest)|{'manifest.json'}
 for name,v in manifest.items():assert sha(out/name)==v['sha256'] and (out/name).stat().st_size==v['size'],name
 print(json.dumps({'passed':True,'capture_sha256':sha(archive),'capture_bytes':archive.stat().st_size,'members':len(manifest)+1,'manifest_sha256':sha(out/'manifest.json'),'complete_dependency_members':{n:len(m)+1 for n,m in deps.items()},'host_proof':host,'scope':'Fresh extraction and offline evidence checks only; native binaries/objects omitted, no GPU or hardware'},indent=2,sort_keys=True))
if __name__=='__main__':main()
