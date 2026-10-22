import json,subprocess,sys,time,pathlib,hashlib,os
os.environ.setdefault('CUDA_DEVICE_ORDER','PCI_BUS_ID')
r=pathlib.Path(os.environ.get('ISSUE236_ROOT','/tmp/issue236-fresh'));variant=sys.argv[1];seed=sys.argv[2];out=r/(variant+'-s'+seed);out.mkdir(exist_ok=True)
opts={'default':[],'no-congestion':['--set','placerHeap/congestion=0'],'arrival':['--arrival-pin-assignment'],'isolated':['--isolated-lut-placement'],'both':['--isolated-lut-placement','--arrival-pin-assignment']}
cmd=[os.environ.get('ISSUE236_NEXTPNR','/tmp/issue236-build/nextpnr-mistral'),'--json',str(r/'in/synth.json'),'--device','5CSEBA6U23I7','--qsf',str(r/'in/socket.qsf'),'--sdc',str(r/'in/clocks-oss.sdc'),'--seed',seed,'--placer-heap-timingweight','300','--placer-heap-critexp','5','--freq','74.25','--router','gpu','--gpu-device','0','--rbf',str(out/'core.rbf'),'--compress-rbf','--write',str(out/'routed.json'),'--report',str(out/'timing.json'),'--timing-report-paths','16']+opts[variant]
(out/'argv.json').write_text(json.dumps(cmd,indent=2));print('START',variant,seed,{k:os.environ.get(k) for k in ['FES_GPU_IDS','GPU_RUN_EXPECTED_DEVICE','CUDA_DEVICE_ORDER','CUDA_VISIBLE_DEVICES','ROCR_VISIBLE_DEVICES']},flush=True);start=time.time()
with (out/'route.log').open('w') as f:rc=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT).returncode
result={'variant':variant,'seed':int(seed),'exit_code':rc,'seconds':time.time()-start,'binary_sha256':hashlib.sha256(pathlib.Path(cmd[0]).read_bytes()).hexdigest()}
if (out/'timing.json').exists():
 d=json.loads((out/'timing.json').read_text());result.update({k:d[k] for k in ['fmax','timing_summary']})
(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
sys.exit(rc)
