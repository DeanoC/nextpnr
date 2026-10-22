#!/usr/bin/env python3
import hashlib,json,os,platform,subprocess,sys,time
from pathlib import Path
root=Path(__file__).resolve().parent
repo=Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr')
runner_root=Path('/home/deano/harnesses/worktrees/fes/silver-delta/out/dev/pcxt-board/sources/misteross')
sys.path[:0]=[str(runner_root),str(runner_root/'scripts')]
from fes_build_common import _run_command
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
preflight=json.loads((root/'preflight.json').read_text())
assert not (root/'route/receipt.json').exists(), 'never overwrite a prior run'
assert preflight['remaining_route_seconds_before']>=600
for name,digest in preflight['inputs'].items():assert sha(root/'inputs'/name)==digest
command=[str(root/'inputs/nextpnr-mistral'),'--device','5CSEBA6U23I7','--json',str(root/'inputs/diagnostic-placed.json'),'--qsf',str(root/'inputs/constraints.qsf'),'--sdc',str(root/'inputs/clocks.sdc'),'--seed','1','--freq','100','--router','gpu','--placer-heap-timingweight','2000','--placer-heap-critexp','5','--no-pack','--no-place','--rbf','diagnostic.rbf','--compress-rbf','--write','routed.json','--report','timing.json','--detailed-timing-report','--timing-report-paths','1024','--routed-endpoints','endpoints.json']
env=dict(os.environ,FES_GPU_LEASE_OWNER='pcxt-agent',GPU_LEASE_MAX_PER_OWNER='2',NEXTPNR_REQUIRE_GPU='1')
receipt=dict(command=command,host=platform.node(),compiler_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),binary_sha256=sha(root/'inputs/nextpnr-mistral'),environment={k:v for k,v in env.items() if k.startswith(('NEXTPNR_','GPU_LEASE_','FES_GPU_'))},timeout_active_seconds=600,remaining_route_seconds_before=preflight['remaining_route_seconds_before'],physical_signoff=False)
(root/'invocation.json').write_text(json.dumps(receipt,indent=2)+'\n')
start=time.monotonic()
try:
 with (root/'route/nextpnr.log').open('w') as log:
  result=_run_command(command,cwd=root/'route',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600,gpu_label='pcxt265-waveform-20261022')
 receipt['returncode']=result.returncode
except Exception as e:receipt['exception']=repr(e)
finally:
 receipt['charged_wall_seconds']=time.monotonic()-start
 receipt['remaining_route_seconds_after']=receipt['remaining_route_seconds_before']-receipt['charged_wall_seconds']
 receipt['artifacts']={p.name:sha(p) for p in sorted((root/'route').iterdir()) if p.is_file()}
 (root/'route/receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
