from pathlib import Path
import json,os,subprocess,time,hashlib
root=Path('/home/deano/tmp/issue236-riscv-20261021')
base=Path('/home/deano/tmp/k267/fes-base/sources/misteross')
synth=Path('/home/deano/tmp/fixjob-nextpnr-267-20261021-055803/build-pr267/riscv-match/synth-base.json')
out=root/'fresh-e904-seed1';out.mkdir(exist_ok=True)
cmd=[str(root/'base-build/nextpnr-mistral'),'--json',str(synth),'--device','5CSEBA6U23I7','--qsf',str(base/'cores/fes-pong/constraints.qsf'),'--sdc',str(base/'boards/de10nano/clocks.sdc'),'--seed','1','--placer-heap-timingweight','10','--placer-heap-critexp','2','--freq','74.25','--router','gpu','--report',str(out/'timing.json'),'--write',str(out/'routed.json'),'--rbf',str(out/'core.rbf'),'--compress-rbf','--detailed-timing-report']
env={'PATH':'/usr/bin:/bin','LANG':'C','LC_ALL':'C','TZ':'UTC','HOME':str(out/'home')};(out/'home').mkdir(exist_ok=True)
start=time.time()
with (out/'route.log').open('w') as f: rc=subprocess.run(cmd,cwd=base,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
hash=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
r={'argv':cmd,'environment':env,'binary_sha256':hash(Path(cmd[0])),'synth_sha256':hash(synth),'exit':rc,'seconds':time.time()-start,'evidence_class':'CPU-reference route comparison; not production qualification'}
if (out/'timing.json').exists():
 t=json.loads((out/'timing.json').read_text());r.update(fmax=t['fmax'],timing_summary=t['timing_summary'])
(out/'result.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
