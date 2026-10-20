#!/usr/bin/env python3
"""Private explicit adaptation of fixture/replay.py for paired compiler builds.
Hash enforcement is replaced with recorded binary/source identity. Frozen input
hash enforcement, clocks, flags, live GPU lease and 600 s timeout are retained.
"""
import argparse, gzip, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--fixture-dir',type=Path,required=True);p.add_argument('--runner-root',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--input',type=Path);p.add_argument('--guidance',type=Path);p.add_argument('--budget',type=int,default=0);p.add_argument('--host-only',action='store_true');p.add_argument('--reload',action='store_true');p.add_argument('--paths',type=int,default=64);a=p.parse_args()
here=a.fixture_dir.resolve(); fixture=json.loads((here/'fixture.json').read_text())
for key in ('GPU_LEASE_IDS','GPU_LEASE_VENDOR','HIP_VISIBLE_DEVICES','CUDA_VISIBLE_DEVICES','ROCR_VISIBLE_DEVICES'):
 assert not os.environ.get(key),key
binary=a.binary.resolve();out=a.output.resolve();out.mkdir(exist_ok=False,parents=True)
raw=gzip.decompress((here/'placed.json.gz').read_bytes());assert hashlib.sha256(raw).hexdigest()==fixture['checkpoint_sha256']
checkpoint=out/'placed.json';checkpoint.write_bytes(a.input.read_bytes() if a.input else raw)
command=fixture['original_command'].copy();command[0]=str(binary)
for opt,path in [('--json',checkpoint),('--qsf',here/'constraints.qsf'),('--sdc',here/'clocks.sdc')]:command[command.index(opt)+1]=str(path)
command[command.index('--timing-report-paths')+1]=str(a.paths)
if a.reload:
 command+=['--no-route'];command[command.index('--timing-report-paths')+1]='1024'
if a.budget:command+=['--critical-swap-budget',str(a.budget),'--critical-cohort-report',str(a.guidance.resolve())]
if a.host_only:
 for opt in ('--rbf','--report','--timing-report-paths'):
  i=command.index(opt);del command[i:i+2]
 command.remove('--compress-rbf');command.remove('--detailed-timing-report');command+=['--no-route']
source=a.source.resolve();git=lambda *args:subprocess.check_output(['git','-C',str(source),*args],text=True).strip()
env=dict(os.environ,FES_GPU_LEASE_OWNER='pcxt-agent',GPU_LEASE_MAX_PER_OWNER='2',NEXTPNR_REQUIRE_GPU='1')
record={'command':command,'source_commit':git('rev-parse','HEAD'),'source_diff_sha256':hashlib.sha256(git('diff','HEAD').encode()).hexdigest(),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'input_sha256':hashlib.sha256(checkpoint.read_bytes()).hexdigest(),'timeout_seconds':600,'environment':{k:env[k] for k in ('FES_GPU_LEASE_OWNER','GPU_LEASE_MAX_PER_OWNER','NEXTPNR_REQUIRE_GPU')},'host_only':a.host_only,'status':'running'}
def save(): (out/'receipt.json').write_text(json.dumps(record,indent=2)+'\n')
save();start=time.monotonic()
root=a.runner_root.resolve();sys.path[:0]=[str(root),str(root/'scripts')]
from fes_build_common import _run_command
try:
 with (out/'nextpnr.log').open('w') as log:
  if a.host_only or a.reload:
   result=subprocess.run(command,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600)
  else:
   result=_run_command(command,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600,gpu_label='pcxt265-'+out.name)
 record.update(status='completed' if result.returncode==0 else 'failed',returncode=result.returncode)
except subprocess.TimeoutExpired:record['status']='timeout'
except Exception as e:record.update(status='error',error=str(e))
record['wall_seconds']=time.monotonic()-start
record['charged_seconds']=0 if a.host_only else record['wall_seconds'] # conservatively includes waits
record['artifacts']={n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in ('routed.json','timing.json','diagnostic.rbf','nextpnr.log') if (out/n).exists()}
if (out/'timing.json').exists():
 t=json.loads((out/'timing.json').read_text());record.update(fmax=t['fmax'],timing_summary=t['timing_summary'])
save();print(record['status'],record['wall_seconds'])

raise SystemExit(0 if record["status"] == "completed" else 1)
