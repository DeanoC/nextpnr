#!/usr/bin/env python3
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--reference-root',type=Path,default=Path('.'));p.add_argument('--nextpnr',required=True,type=Path);p.add_argument('--yosys',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
# The small fixture and pin order are independent of the PCXT checkpoint.
base=a.reference_root/'dq-native30';q=(base/'top.qsf').read_text().splitlines();q=[line for line in q if not any('-name '+v+' ' in line for v in ('D1_DELAY','D3_DELAY','D5_DELAY','D5_OE_DELAY'))]
for i in range(16):
 for k,v in [('NEXTPNR_GPIO_TIMING_PROFILE','QUARTUS_17_0_2_DQ_UNREGISTERED'),('NEXTPNR_INPUT_REG4_SEL','BYPASS')]:q.append(f'set_instance_assignment -name {k} {v} -to dq[{i}]')
(out/'pins.qsf').write_text('\n'.join(q)+'\n');(out/'clocks.sdc').write_text((base/'clocks.sdc').read_text().replace('-period 20','-period 100'))
def run(cmd,name,expected=0):
 with (out/(name+'.log')).open('w') as f:r=subprocess.run([str(x) for x in cmd],stdout=f,stderr=subprocess.STDOUT,timeout=300)
 assert r.returncode==expected,(name,r.returncode)
 if expected:assert 'Hold/min time violation' in (out/(name+'.log')).read_text()
run([a.yosys,'-p',f'read_verilog {base.resolve()}/top.v; synth_intel_alm -nobram -nodsp -top top; write_json {out}/synth.json'],'synth')
run([a.nextpnr,'--device','5CSEBA6U23I7','--json',out/'synth.json','--qsf',out/'pins.qsf','--sdc',out/'clocks.sdc','--router','router2','--seed','1','--write',out/'routed.json','--report',out/'report.json','--detailed-timing-report','--rbf',out/'top.rbf'],'route',1)
run([a.nextpnr,'--device','5CSEBA6U23I7','--json',out/'routed.json','--no-pack','--no-place','--no-route','--report',out/'reloaded-report.json','--detailed-timing-report','--rbf',out/'reloaded.rbf'],'reload',1)
assert (out/'top.rbf').read_bytes()==(out/'reloaded.rbf').read_bytes()
assert json.loads((out/'report.json').read_text())['critical_paths']==json.loads((out/'reloaded-report.json').read_text())['critical_paths']
(out/'receipt.json').write_text(json.dumps(dict(status='rejected-hold',route_exit_code=1,replay_exit_code=1,scope='small synthetic100ns fixture only; no timing acceptance',replay_bit_exact=True,physical_signoff=False),indent=2)+'\n')
print('PASS: unchanged synthetic hold rejection and exact RBF/report replay; route remains nonpassing')
