import json,subprocess,sys,time,pathlib,hashlib,os
os.environ.setdefault('CUDA_DEVICE_ORDER','PCI_BUS_ID')
r=pathlib.Path(os.environ.get('ISSUE236_ROOT','/tmp/issue236-closure'));variant=sys.argv[1];seed=sys.argv[2];out=r/(variant+'-s'+seed);out.mkdir(exist_ok=True)
opts={'w1000':['--arrival-pin-assignment','--placer-heap-timingweight','1000'], 'w2000':['--arrival-pin-assignment','--placer-heap-timingweight','2000'], 'arrival-no-congestion':['--arrival-pin-assignment','--set','placerHeap/congestion=0'], 'default':[],'no-congestion':['--set','placerHeap/congestion=0'],'arrival':['--arrival-pin-assignment'],'isolated':['--isolated-lut-placement'],'both':['--isolated-lut-placement','--arrival-pin-assignment']}
cmd=[os.environ.get('ISSUE236_NEXTPNR','/tmp/issue236-closure/bin/nextpnr-mistral'),'--json',str(r/'in/synth.json'),'--device','5CSEBA6U23I7','--qsf',str(r/'in/socket.qsf'),'--sdc',str(r/'in/clocks-oss.sdc'),'--seed',seed,'--placer-heap-timingweight','300','--placer-heap-critexp','5','--freq','74.25','--router','gpu','--gpu-device','0','--rbf',str(out/'core.rbf'),'--compress-rbf','--write',str(out/'routed.json'),'--report',str(out/'timing.json'),'--timing-report-paths','16']+[x for x in opts[variant] if x not in ['--placer-heap-timingweight','1000','2000']]

if variant in ['w1000','w2000']: cmd[cmd.index('--placer-heap-timingweight')+1]=variant[1:]
(out/'argv.json').write_text(json.dumps(cmd,indent=2));print('START',variant,seed,{k:os.environ.get(k) for k in ['FES_GPU_IDS','GPU_RUN_EXPECTED_DEVICE','CUDA_DEVICE_ORDER','CUDA_VISIBLE_DEVICES','ROCR_VISIBLE_DEVICES']},flush=True);start=time.time();binary_hash=hashlib.sha256(pathlib.Path(cmd[0]).read_bytes()).hexdigest()
with (out/'route.log').open('w') as f:rc=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT).returncode
result={'variant':variant,'seed':int(seed),'exit_code':rc,'seconds':time.time()-start,'binary_sha256':binary_hash}
if (out/'timing.json').exists():
 d=json.loads((out/'timing.json').read_text());result.update({k:d[k] for k in ['fmax','timing_summary']})
(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
sys.exit(rc)
