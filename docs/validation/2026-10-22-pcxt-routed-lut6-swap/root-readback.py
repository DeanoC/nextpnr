from pathlib import Path
import hashlib,json,subprocess,tarfile
ROOT=Path(__file__).resolve().parent
CAP=ROOT/'capture'
DEST=ROOT/'root-extraction'
DEST.mkdir(exist_ok=False)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
arc=CAP/'capture.tar.gz'
assert sha(arc)=='1b9642742873e2c6d4c14aba4d234fdddea2bd9e0b3f1b1ee030cbfd5230d4f6'
with tarfile.open(arc,'r:gz') as tf:
 members=tf.getmembers(); names=[m.name for m in members]
 assert len(members)==len(set(names))==103
 for m in members:
  p=Path(m.name)
  assert m.isfile() and not p.is_absolute() and '..' not in p.parts,m.name
 tf.extractall(DEST,filter='data')
m=json.loads((DEST/'manifest.json').read_text())
assert set(names)==set(m)|{'manifest.json'}
assert sha(DEST/'manifest.json')=='d4b994340dce22038f924df1fd1d511f72812e26ec7358961afc29ee38751c87'
for name,record in m.items():
 p=DEST/name;assert p.stat().st_size==record['size'] and sha(p)==record['sha256'],name
steps=[('verify_sources.py',['source-control-proof.json']),('test_checks.py',['checker-tests.json']),('prove_graph.py',['graph-proof.json','decision-ledger.json']),('compare_timing.py',['route-comparison.json']),('summarize.py',['summary.json'])]
results=[]
for script,outputs in steps:
 before={name:(DEST/'final-analysis'/name).read_bytes() for name in outputs}
 with (CAP/('root-'+script+'.log')).open('w') as log:
  code=subprocess.run(['python3',str(DEST/'final-analysis'/script)],cwd=DEST,stdout=log,stderr=subprocess.STDOUT).returncode
 assert code==0,(script,code)
 for name in outputs:assert (DEST/'final-analysis'/name).read_bytes()==before[name],name
 results.append({'script':script,'returncode':code,'outputs_byte_identical':outputs})
result={'status':'pass','all_members_verified':len(members),'archive_sha256':sha(arc),'manifest_sha256':sha(DEST/'manifest.json'),'checks':results,'route_charge':0,'hardware':False}
(CAP/'root-readback.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
