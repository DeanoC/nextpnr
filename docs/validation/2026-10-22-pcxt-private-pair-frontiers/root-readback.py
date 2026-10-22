from pathlib import Path
import hashlib,json,subprocess,tarfile
ROOT=Path(__file__).resolve().parent
CAP=ROOT/'capture'
DEST=ROOT/'root-extraction'
DEST.mkdir(exist_ok=False)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
arc=CAP/'capture.tar.gz'
assert sha(arc)=='4c82c3d307ad1148abc7dfffdd1d1c0efac58dcb93b419cfac3f9fb02cafba91'
with tarfile.open(arc,'r:gz') as tf:
 members=tf.getmembers(); names=[m.name for m in members]
 assert len(members)==len(set(names))==308
 for m in members:
  p=Path(m.name)
  assert m.isfile() and not p.is_absolute() and '..' not in p.parts,m.name
 tf.extractall(DEST,filter='data')
m=json.loads((DEST/'manifest.json').read_text())
assert set(names)==set(m)|{'manifest.json'}
assert sha(DEST/'manifest.json')=='e65bf2caa985766367df5ba2e811084d87767506831a2c764d17d5f111794f86'
for name,record in m.items():
 p=DEST/name;assert p.stat().st_size==record['size'] and sha(p)==record['sha256'],name
steps=[('hydrate.py',[]),('prove_no_trial.py',['no-trial-proof.json']),('compare_timing.py',['route-comparison.json']),('verify_frontiers.py',['frontier-proof.json'])]
results=[]
for script,outputs in steps:
 before={name:(DEST/'final-analysis'/name).read_bytes() for name in outputs}
 with (CAP/('root-'+script+'.log')).open('w') as log:
  command=['python3',str(DEST/'final-analysis'/script)]
  if script=='hydrate.py':command.append(str(ROOT.parent.parent/'lut6-source-coverage-20261022/final-analysis/capture/capture.tar.gz'))
  code=subprocess.run(command,cwd=DEST,stdout=log,stderr=subprocess.STDOUT).returncode
 assert code==0,(script,code)
 for name in outputs:assert (DEST/'final-analysis'/name).read_bytes()==before[name],name
 results.append({'script':script,'returncode':code,'outputs_byte_identical':outputs})
result={'status':'pass','all_members_verified':len(members),'archive_sha256':sha(arc),'manifest_sha256':sha(DEST/'manifest.json'),'checks':results,'route_charge':0,'hardware':False}
(CAP/'root-readback.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
