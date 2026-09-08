#!/usr/bin/env python3
"""Infer and route one true dual-port M10K with two enabled read/write ports."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


def run(command, log, timeout=120):
    with log.open('w') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=timeout)


def diagnostics(args, design, name, out):
    for label, params, disconnect, message in (
        ('width40', {'CFG_DBITS':40}, None, 'true dual-port requires'),
        ('address', {'CFG_ABITS':8}, None, 'true dual-port requires'),
        ('mixed', {'CFG_MIXED_WIDTH':1}, None, 'mixed TDP requires explicit B geometry'),
        ('byte-enable', {'CFG_BYTE_ENABLE':1}, None, 'byte mode requires connected'),
        ('clock', {}, 'CLK1', 'both clocks'),
    ):
        bad=copy.deepcopy(design)
        cell=bad['modules']['top']['cells'][name]
        cell['parameters'].update({k:format(v,'032b') for k,v in params.items()})
        if disconnect: cell['connections'][disconnect]=[]
        path=out/(label+'.json');path.write_text(json.dumps(bad))
        result=subprocess.run([str(args.nextpnr.resolve()),'--device','5CSEBA6U23I7',
             '--qsf',str(args.qsf.resolve()),'--json',str(path)],stdout=subprocess.PIPE,
             stderr=subprocess.STDOUT,text=True,timeout=120)
        (out/(label+'.log')).write_text(result.stdout)
        assert result.returncode and message in result.stdout,(label,result.stdout[-2000:])
    print('PASS: five invalid TDP configurations rejected',flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('yosys','nextpnr','mistral-cv','qsf','sdc','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--case',action='append',help='Run only wWIDTH-cSAME_CLOCK (repeatable)')
    a=p.parse_args();out=a.output.resolve();source=Path(__file__).with_suffix('.v').resolve()
    for width,same in ((10,0),(20,0),(8,0),(16,0),(10,1),(20,1)):
        label=f'w{width}-c{same}'
        if a.case and label not in a.case:continue
        case=out/label;case.mkdir(parents=True,exist_ok=True)
        script=case/'synth.ys';script.write_text(f'read_verilog {source}\n'
             f'chparam -set WIDTH {width} -set SAME_CLOCK {same} top\n'
             'synth_intel_alm -nolutram -nodsp -top top\n'
             'select -assert-count 1 t:MISTRAL_M10K_TDP\n'
             f'write_json {case/"synth.json"}\n')
        run([str(a.yosys.resolve()),'-Q','-T','-s',str(script)],case/'synth.log')
        design=json.loads((case/'synth.json').read_text())
        name,cell=next((n,c) for n,c in design['modules']['top']['cells'].items() if c['type']=='MISTRAL_M10K_TDP')
        physical=10 if width<=10 else 20
        assert int(cell['parameters']['CFG_DBITS'],2)==physical
        assert int(cell['parameters']['CFG_ABITS'],2)==(10 if physical==10 else 9)
        if (width,same)==(20,0):diagnostics(a,design,name,case)
        run([str(a.nextpnr.resolve()),'--device','5CSEBA6U23I7','--freq','50',
             '--qsf',str(a.qsf.resolve()),'--sdc',str(a.sdc.resolve()),'--json',str(case/'synth.json'),
             '--compress-rbf','--rbf',str(case/'top.rbf'),'--write',str(case/'routed.json'),
             '--report',str(case/'timing.json')],case/'route.log')
        report=json.loads((case/'timing.json').read_text())
        assert report['utilization']['MISTRAL_M10K']['used']==1
        assert report['utilization']['cyclonev_hps_interface_mpu_general_purpose']['used']==1
        assert report['utilization']['altera_pll']['used']==(0 if same else 1)
        assert report['fmax'] and all(c['achieved']>=c['constraint']==50 for c in report['fmax'].values())
        if not same:
            assert any(path['max_delay'] == 20
                       and any(arc['type'] in ('clk-to-q', 'setup') and arc['from']['cell'] == name
                               and arc['from']['port'].startswith('A1') for arc in path['path'])
                       for path in report['critical_paths']), 'missing A-port register timing'
            assert any(path['from'] == 'posedge clock_b' and path['max_delay'] == 40
                       and any(arc['type'] == 'clk-to-q' and arc['from']['cell'] == name
                               and arc['from']['port'].startswith('B1Q[') for arc in path['path'])
                       for path in report['critical_paths']), 'missing B-port output clock timing'
            assert any(path['to'] == 'posedge clock_b'
                       and any(arc['type'] == 'setup' and arc['to']['cell'] == name
                               and arc['to']['port'].startswith('B1') for arc in path['path'])
                       for path in report['critical_paths']), 'missing B-port input clock timing'
        run([str(a.mistral_cv.resolve()),'decomp','5CSEBA6U23I7',str(case/'top.rbf'),str(case/'top.bt')],case/'decomp.log')
        routed=json.loads((case/'routed.json').read_text())['modules']['top']['cells'][name]
        assert routed['type']=='MISTRAL_M10K' and int(routed['parameters']['CFG_TDP'],2)==1
        _,x,y,_=routed['attributes']['NEXTPNR_BEL'].split('.')
        site=f'M10K.{int(x):03d}.{int(y):03d}';bt=(case/'top.bt').read_text()
        fields=dict(re.findall(r'^s '+re.escape(site)+r':(\S+) (\S+)$',bt,re.M))
        for field in ('TRUE_DUAL_PORT','TOP_CLK_SEL','TOP_CORECLK_SEL','TOP_CE0_SEL','TOP_INCLK_SEL',
                      'BOT_CLK_SEL','BOT_CORECLK_SEL','BOT_INCLK_SEL','BOT_1_CORECLK_SEL',
                      'BOT_1_INCLK_SEL','BOT_1_OUTCLK_SEL','A_DATA_FLOW_THRU','B_DATA_FLOW_THRU'):
            assert fields.get(field,'0')=='1',(field,fields.get(field))
        for field in ('A_DATA_WIDTH','B_DATA_WIDTH'):assert fields[field]==str(physical)
        for pin in ('CLKIN.0','CLKIN.1','WREN.0','WREN.1','ENABLE.0','ENABLE.1'):
            assert re.search(r'^r \S+ '+re.escape(site+':'+pin)+r'$',bt,re.M),pin
        print(f'PASS: {label}: one TDP M10K, both ports, selectors, RBF and 50 MHz',flush=True)


if __name__=='__main__':main()
