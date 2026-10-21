from pathlib import Path
import hashlib,json,os,subprocess,sys,time
root=Path('/home/deano/tmp/issue236-riscv-20261021')
base=Path('/home/deano/tmp/k267/fes-base/sources/misteross')
pr=Path('/home/deano/tmp/fixjob-nextpnr-267-20261021-055803/build-pr267')
which,route=sys.argv[1:3]
binary={'e904':root/'base-build/nextpnr-mistral','pr':pr/'nextpnr-mistral','9128':Path('/home/deano/tmp/k267/cache-base/slots/fac97e973f94adee154caaaf227d75bc971b6e0ab78bdb4e84f383690f6d6764/install/bin/nextpnr-mistral')}[which]
source={'old':base/'build/fes-riscv/routed.json','new':pr/'riscv-match/pr-seed1/routed.json','basefresh':root/'fresh-e904-seed1/routed.json'}[route]
original={'old':base/'build/fes-riscv/core.rbf','new':pr/'riscv-match/pr-seed1/core.rbf','basefresh':root/'fresh-e904-seed1/core.rbf'}[route]
out=root/f'matrix-{which}-{route}';out.mkdir(parents=True,exist_ok=True)
d=json.loads(source.read_text());d['modules']['top']['settings']['timing/allowFail']='0'
snapshot=out/'input.json';snapshot.write_text(json.dumps(d))
cmd=[str(binary),'--json',str(snapshot),'--device','5CSEBA6U23I7','--qsf',str(base/'cores/fes-pong/constraints.qsf'),'--sdc',str(base/'boards/de10nano/clocks.sdc'),'--freq','74.25','--no-pack','--no-place','--no-route','--rbf',str(out/'core.rbf'),'--compress-rbf','--report',str(out/'timing.json'),'--detailed-timing-report']
if which=='9128':
 i=cmd.index('--sdc'); del cmd[i:i+2]
 # The old SDC parser cannot resolve packed port aliases. The snapshot
 # retains the original clock settings; --freq remains 74.25 MHz.
start=time.time()
with (out/'route.log').open('w') as f:rc=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT).returncode
hash=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
result={'compiler':which,'route':route,'argv':cmd,'source':str(source),'source_sha256':hash(source),'binary_sha256':hash(binary),'exit':rc,'seconds':time.time()-start}
if (out/'core.rbf').exists():result.update(rbf_sha256=hash(out/'core.rbf'),original_rbf_sha256=hash(original),identical_rbf=(out/'core.rbf').read_bytes()==original.read_bytes())
if (out/'timing.json').exists():
 t=json.loads((out/'timing.json').read_text());result.update(fmax=t['fmax'],timing_summary=t.get('timing_summary'))
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='argv'}))
