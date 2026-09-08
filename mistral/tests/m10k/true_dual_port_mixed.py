#!/usr/bin/env python3
"""Infer and route mixed-width M10K TDP RAMs with both ports reading/writing."""
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
    cases = (
        ('a-address', {'CFG_ABITS':8}, None, None, 'true dual-port requires'),
        ('b-width', {'CFG_RD_ABITS':8,'CFG_RD_DBITS':40}, None, None, 'true dual-port requires'),
        ('missing-b', {}, 'CFG_RD_DBITS', None, 'mixed TDP requires explicit B geometry'),
        ('flag', {'CFG_MIXED_WIDTH':0}, None, None, 'requires CFG_MIXED_WIDTH=1'),
        ('byte', {'CFG_BYTE_ENABLE':1}, None, None, 'cannot combine'),
        ('clock', {}, None, 'CLK1', 'both clocks'),
        ('enable', {}, None, 'A1EN', 'requires connected A1EN'),
        ('write', {}, None, 'B1WE', 'requires connected B1WE'),
    )
    for label,params,remove,disconnect,message in cases:
        bad=copy.deepcopy(design);cell=bad['modules']['top']['cells'][name]
        cell['parameters'].update({k:format(v,'032b') for k,v in params.items()})
        if remove:cell['parameters'].pop(remove)
        if disconnect:cell['connections'][disconnect]=[]
        path=out/(label+'.json');path.write_text(json.dumps(bad))
        result=subprocess.run([str(args.nextpnr.resolve()),'--device','5CSEBA6U23I7',
            '--qsf',str(args.qsf.resolve()),'--json',str(path)],stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,text=True,timeout=120)
        (out/(label+'.log')).write_text(result.stdout)
        assert result.returncode and message in result.stdout,(label,result.stdout[-2000:])
    print('PASS: eight invalid mixed TDP configurations rejected',flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('yosys','nextpnr','mistral-cv','qsf','sdc','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--case',action='append',help='Run only uN-aN-bN-cN[-raw] (repeatable)')
    a=p.parse_args();out=a.output.resolve();source=Path(__file__).with_suffix('.v').resolve()
    cases=[(u,al,bl,c,0) for u in (10,8) for al,bl in ((2,1),(1,2)) for c in (0,1)]
    cases += [(10,1,1,0,0),(10,2,2,0,0),(10,2,1,0,1),(10,1,2,0,1)]
    labels = {f'u{u}-a{al}-b{bl}-c{c}' + ('-raw' if raw else '')
              for u,al,bl,c,raw in cases}
    if a.case and set(a.case) - labels:
        p.error(f'unknown cases: {sorted(set(a.case) - labels)}')
    for unit,al,bl,same,raw in cases:
        label=f'u{unit}-a{al}-b{bl}-c{same}'+('-raw' if raw else '')
        if a.case and label not in a.case:continue
        case=out/label;case.mkdir(parents=True,exist_ok=True)
        script=case/'synth.ys';script.write_text(f'read_verilog {source}\n'
            f'chparam -set UNIT {unit} -set ALANES {al} -set BLANES {bl} -set SAME_CLOCK {same} -set RAW {raw} top\n'
            'synth_intel_alm -nolutram -nodsp -top top\n'
            'select -assert-count 1 t:MISTRAL_M10K_TDP\n'
            'select -assert-none t:MISTRAL_M10K t:MISTRAL_MLAB\n'
            f'write_json {case/"synth.json"}\n')
        run([str(a.yosys.resolve()),'-Q','-T','-s',str(script)],case/'synth.log')
        design=json.loads((case/'synth.json').read_text());cells=design['modules']['top']['cells']
        name,cell=next((n,c) for n,c in cells.items() if c['type']=='MISTRAL_M10K_TDP')
        params={k:int(v,2) for k,v in cell['parameters'].items() if k!='INIT'}
        assert params['CFG_MIXED_WIDTH']==1
        gp=next(c for c in cells.values() if c['type']=='cyclonev_hps_interface_mpu_general_purpose')['connections']['gp_out']
        # memory_libmap can exchange the two physical read/write ports.
        physical_a_is_a=cell['connections']['A1EN']==[gp[28]]
        assert cell['connections']['A1EN']==[gp[28 if physical_a_is_a else 29]]
        assert cell['connections']['B1EN']==[gp[29 if physical_a_is_a else 28]]
        da,db=(10*al,10*bl) if physical_a_is_a else (10*bl,10*al)
        assert (params['CFG_DBITS'],params['CFG_RD_DBITS'])==(da,db)
        assert (params['CFG_ABITS'],params['CFG_RD_ABITS'])==(10 if da==10 else 9,10 if db==10 else 9)
        assert (cell['connections']['CLK1']==cell['connections']['CLK2'])==bool(same)
        if raw:assert physical_a_is_a
        if (unit,al,bl,same,raw)==(10,2,1,0,0):diagnostics(a,design,name,case)
        run([str(a.nextpnr.resolve()),'--device','5CSEBA6U23I7','--freq','50',
            '--qsf',str(a.qsf.resolve()),'--sdc',str(a.sdc.resolve()),'--json',str(case/'synth.json'),
            '--compress-rbf','--rbf',str(case/'top.rbf'),'--write',str(case/'routed.json'),
            '--report',str(case/'timing.json')],case/'route.log')
        report=json.loads((case/'timing.json').read_text())
        assert report['utilization']['MISTRAL_M10K']['used']==1
        assert report['utilization']['cyclonev_hps_interface_mpu_general_purpose']['used']==1
        assert report['utilization']['altera_pll']['used']==(0 if same else 1)
        assert report['fmax'] and all(c['achieved']>=c['constraint'] for c in report['fmax'].values())
        assert any(c['constraint']==50 for c in report['fmax'].values())
        if not same:
            bprefix='B1' if physical_a_is_a else 'A1'
            assert any(path['from']=='posedge clock_b' and path['max_delay']==40
                and any(arc['type']=='clk-to-q' and arc['from']['cell']==name
                    and arc['from']['port'].startswith(bprefix+'Q[') for arc in path['path'])
                for path in report['critical_paths']), 'missing logical B 25 MHz output timing'
            assert any(path['to']=='posedge clock_b'
                and any(arc['type']=='setup' and arc['to']['cell']==name
                    and arc['to']['port'].startswith(bprefix) for arc in path['path'])
                for path in report['critical_paths']), 'missing logical B input timing'
        run([str(a.mistral_cv.resolve()),'decomp','5CSEBA6U23I7',str(case/'top.rbf'),str(case/'top.bt')],case/'decomp.log')
        packed=json.loads((case/'routed.json').read_text())['modules']['top']['cells'][name]
        assert packed['type']=='MISTRAL_M10K' and int(packed['parameters']['CFG_TDP'],2)==1
        _,x,y,_=packed['attributes']['NEXTPNR_BEL'].split('.')
        site=f'M10K.{int(x):03d}.{int(y):03d}';bt=(case/'top.bt').read_text()
        fields=dict(re.findall(r'^s '+re.escape(site)+r':(\S+) (\S+)$',bt,re.M))
        if da!=db:
            oracle=source.parent/'oracle'/'tdp-mixed'/f'a{da}-b{db}'/'mapping.json'
        else:
            oracle=source.parent/'oracle'/f'tdp{da}'/'mapping.json'
        expected=json.loads(oracle.read_text())['settings']
        # INIT differs from the uninitialized oracle and is checked by SAT/kit readback.
        setting_names = {k for k in fields.keys() | expected.keys() if not k.startswith('RAM.')}
        for key in setting_names:
            assert fields.get(key,'0')==expected.get(key,'0'),(label,key,fields.get(key),expected.get(key))
        for pin in ('CLKIN.0','CLKIN.1','WREN.0','WREN.1','ENABLE.0','ENABLE.1'):
            assert re.search(r'^r \S+ '+re.escape(site+':'+pin)+r'$',bt,re.M),pin
        print(f'PASS: {label}: one TDP, physical A{da}/B{db}, routing, oracle settings, RBF and timing',flush=True)


if __name__=='__main__':main()
