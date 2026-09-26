#!/usr/bin/env python3
"""Check dedicated DDR clock forwarding; does not program or measure a pin."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
for name in ('yosys','nextpnr','mistral-cv','output'):
    p.add_argument('--'+name,required=True,type=Path)
a=p.parse_args();fixture=Path(__file__).resolve().parent;out=a.output.resolve()

def run(cmd,log,success=True):
    result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
    log.write_text(result.stdout)
    assert (result.returncode==0)==success,(cmd,result.stdout[-2000:])
    return result.stdout

for name,pll,inv,minimum in [('direct',0,0,0),('inverted',0,1,0),('minimal',0,0,1),('pll7425',1,0,0),('pll-minimal',1,0,1)]:
    case=out/name;case.mkdir(parents=True,exist_ok=True)
    script=case/'synth.ys';script.write_text(f'read_verilog {fixture/"top.v"}\n'
        f'chparam -set WITH_PLL {pll} -set INVERTED {inv} -set MINIMAL {minimum} top\n'
        'synth_intel_alm -nobram -nolutram -nodsp -top top\n'
        'select -assert-count 1 t:altddio_out\n'+f'write_json {case/"synth.json"}\n')
    run([str(a.yosys.resolve()),'-s',str(script)],case/'synth.log')
    command=[str(a.nextpnr.resolve()),'--device','5CSEBA6U23I7','--qsf',str(fixture/'pins.qsf'),
             '--sdc',str(fixture/'clocks.sdc'),'--freq','50']
    run(command+['--json',str(case/'synth.json'),'--compress-rbf','--rbf',str(case/'top.rbf'),
        '--write',str(case/'routed.json'),'--report',str(case/'timing.json')],case/'route.log')
    packed=json.loads((case/'routed.json').read_text())['modules']['top']['cells']
    ddr=[c for c in packed.values() if c['type']=='MISTRAL_DDROUT'];assert len(ddr)==1
    assert not any(c['type']=='altddio_out' for c in packed.values())
    assert ddr[0]['connections']['CLK']
    assert int(ddr[0]['parameters']['DDR_HIGH'],2)==1-inv
    report=json.loads((case/'timing.json').read_text())
    assert report['utilization']['MISTRAL_IO']['used']==2
    assert report['utilization']['altera_pll']['used']==pll
    if not minimum:
        assert report['fmax'] and all(c['achieved']>=c['constraint'] for c in report['fmax'].values())
        if pll:assert any(abs(c['constraint']-74.25)<0.001 for c in report['fmax'].values())
    else:
        assert not any(c['type']=='MISTRAL_FF' for c in packed.values())
    run([str(a.mistral_cv.resolve()),'decomp','5CSEBA6U23I7',str(case/'top.rbf'),str(case/'top.bt')],case/'decomp.log')
    bt=(case/'top.bt').read_text()
    # Exact per-pad DDR configuration from the retained Quartus W15 oracle.
    fields=dict(re.findall(r'^s DQS16\.089\.008:(\S+\.9) (\S+)',bt,re.M))
    expected={'OUTREG_MODE_SEL.9':'DDR','OUTREG_OUTPUT_SEL.9':'SEL_SDR_DELAY','RBOE_LVL_FR_CLK_EN.9':'1'}
    assert fields==expected,(name,fields)
    assert f'i GPIO.089.008.1:DATAOUT.{0 if inv else 1} 1' in bt
    assert f'i GPIO.089.008.1:DATAOUT.{1 if inv else 0} 1' not in bt
    # Route node names can be HMC bypass nodes; inspect the semantic route list.
    routes=run([str(a.mistral_cv.resolve()),'routes','5CSEBA6U23I7',str(case/'top.rbf')],case/'routes.txt')
    assert 'GPIO.089.008.1:CLKOUT.0' in routes
    print('PASS:',name,'one DDR output, clock route, polarity, oracle settings and RBF',flush=True)
    if name!='direct':continue
    design=json.loads((case/'synth.json').read_text())
    cases=[('width',{'width':2},{},'DDR output requires width=1'),
           ('data',{}, {'datain_h':design['modules']['top']['cells']['ddr']['connections']['outclock']},'fabric DDR data requires'),
           ('equal',{}, {'datain_l':['1']},'fabric DDR data requires'),
           ('clock',{}, {'outclock':[]},'outclock must'),
           ('clock-constant',{}, {'outclock':['0']},'outclock must'),
           ('reset',{}, {'aclr':['1']},'resets constant low'),
           ('oe',{}, {'oe':['0']},'OE must'),
           ('enable',{}, {'outclocken':['0']},'enable/OE'),
           ('invert',{'invert_output':'ON'}, {},'unsupported parameter')]
    for label,params,ports,message in cases:
        bad=copy.deepcopy(design);cell=bad['modules']['top']['cells']['ddr']
        cell['parameters'].update({k:format(v,'032b') if isinstance(v,int) else v for k,v in params.items()})
        cell['connections'].update(ports)
        path=case/(label+'.json');path.write_text(json.dumps(bad))
        log=run(command+['--json',str(path)],case/(label+'.log'),False)
        assert message in log,(label,log[-1500:])
    print('PASS: nine invalid DDR requests rejected',flush=True)
