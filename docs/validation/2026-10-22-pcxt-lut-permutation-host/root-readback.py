from pathlib import Path
import hashlib,json,subprocess,tarfile
ROOT=Path(__file__).resolve().parent
CAP=ROOT/'capture';DEST=ROOT/'root-extraction';DEST.mkdir(exist_ok=False)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
arc=CAP/'capture.tar.gz'
assert sha(arc)=='3a9da14c90439588794b1c301df1357a28c15718ea8c729723fb7130e6c8d130'
with tarfile.open(arc,'r:gz') as tf:
 members=tf.getmembers();names=[m.name for m in members];assert len(names)==len(set(names))==105
 for m in members:
  p=Path(m.name);assert m.isfile() and not p.is_absolute() and '..' not in p.parts
 tf.extractall(DEST,filter='data')
m=json.loads((DEST/'manifest.json').read_text())
assert set(names)==set(m)|{'manifest.json'}
assert sha(DEST/'manifest.json')=='4d413aa1c00d2159cc47aec74c19c44015bd06b2d1d8ac3bb760884ca0b5f149'
for n,v in m.items():assert (DEST/n).stat().st_size==v['size'] and sha(DEST/n)==v['sha256'],n
base=ROOT.parent.parent
steps=[('final-analysis/hydrate.py',[str(base/'routed-private-pair-board-20261022/final-analysis/capture/capture.tar.gz'),str(base/'lut6-source-coverage-20261022/final-analysis/capture/capture.tar.gz')],[]),('final-analysis/verify_host.py',[],['final-analysis/host-proof.json']),('dependency/frontier/final-analysis/verify_frontiers.py',[],['dependency/frontier/final-analysis/frontier-proof.json'])]
results=[]
for script,args,outputs in steps:
 before={n:(DEST/n).read_bytes() for n in outputs}
 with (CAP/('root-'+Path(script).name+'.log')).open('w') as log:
  code=subprocess.run(['python3',str(DEST/script),*args],cwd=DEST,stdout=log,stderr=subprocess.STDOUT).returncode
 assert code==0,(script,code)
 for n in outputs:assert (DEST/n).read_bytes()==before[n],n
 results.append({'script':script,'returncode':code,'outputs_byte_identical':outputs})
result={'status':'pass','all_members_verified':len(members),'archive_sha256':sha(arc),'manifest_sha256':sha(DEST/'manifest.json'),'checks':results,'route_charge':0,'hardware':False}
(CAP/'root-readback.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
