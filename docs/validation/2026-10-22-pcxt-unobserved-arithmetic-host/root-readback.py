from pathlib import Path
import hashlib,json,os,subprocess,sys,time
ROOT=Path(__file__).resolve().parent
DOC=Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr/docs/validation')
CAP=DOC/'2026-10-22-pcxt-unobserved-arithmetic-host'
def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
archive=CAP/'capture.tar.gz'
assert sha(archive)=='bb96052b78d487b127e105a326be221c7c6e5e1b0082d68a2418b89af0c4cce1'
command=[sys.executable,str(CAP/'verify_capture.py'),str(archive),str(ROOT/'root-fresh-extraction'),str(DOC/'2026-10-22-pcxt-lut-permutation-host/capture.tar.gz'),str(DOC/'2026-10-22-pcxt-private-pair-frontiers/capture.tar.gz'),str(DOC/'2026-10-22-pcxt-lut6-source-coverage/capture.tar.gz')]
start=time.monotonic()
with (ROOT/'root-readback.log').open('w') as f:rc=subprocess.run(command,stdout=f,stderr=subprocess.STDOUT,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1')).returncode
receipt={'command':command,'returncode':rc,'elapsed_seconds':time.monotonic()-start,'script_sha256':sha(__file__),'verifier_sha256':sha(CAP/'verify_capture.py'),'archive_sha256':sha(archive),'log_sha256':sha(ROOT/'root-readback.log'),'evidence':'Independent root fresh-extraction offline replay; zero route charge, no native rerun or hardware.'}
(ROOT/'root-readback.json').write_text(json.dumps(receipt,indent=2)+'\n')
assert rc==0,(ROOT/'root-readback.log').read_text()
result=json.loads((ROOT/'root-readback.log').read_text());assert result['passed'] is True and result['members']==173
assert result['manifest_sha256']=='74a0ed0904b2fa37dd078fc7a04e72917e17b65707ebe18829ad49eed2c83178'
print(json.dumps({k:result[k] for k in ['passed','members','complete_dependency_members','capture_sha256']},indent=2))
