#!/usr/bin/env python3
"""Audit bounded dynamic-CE reference fits, never whole-board timing signoff."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import sys

# May run from output root or from the committed validation directory.
REPO = next((p for p in Path(__file__).resolve().parents if (p/'mistral/tests/gpio-timing').is_dir()),
            Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr'))
sys.path.insert(0, str(REPO/'mistral/tests/gpio-timing'))
from characterize import paths
from pad_summary import summarize as pads
from clock_summary import summarize as clocks

PINS = dict(zip([f'SDRAM_A[{i}]' for i in range(11)]+['SDRAM_BA[0]', 'SDRAM_BA[1]'],
                ['Y11','AA26','AA13','AA11','W11','Y19','AB23','AC23','AC22','C12','AB26','Y17','AB25']))
CORNERS = {'7_slow_1100mv_-40c', '7_slow_1100mv_100c', 'MIN_fast_1100mv_-40c', 'MIN_fast_1100mv_100c'}
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def audit(root):
    result = {}
    for load in (0, 30):
        project = root/f'reference{load}'
        evidence = json.loads((project/'evidence.json').read_text())
        assert evidence['device']=='5CSEBA6U23I7' and evidence['io_standard']=='3.3-V LVTTL'
        assert evidence['output_load_pf']==load and evidence['all_transitions'] is True
        assert set(evidence['variants'])=={'dynamic-ce'}
        variant=evidence['variants']['dynamic-ce']
        for name,digest in variant['hashes'].items():
            assert sha(project/name)==digest, name
        assert {r.split('/')[0] for r in variant['reports']}==CORNERS
        for report, rows in variant['reports'].items():
            assert paths((project/report).read_text())==rows, report
        qsf=(project/'top.qsf').read_text(); rtl=(project/'top.v').read_text()
        assert 'if (beat[15]) begin SDRAM_A <= beat[10:0]; SDRAM_BA <= beat[12:11]; end' in rtl
        assert 'DEVICE 5CSEBA6U23I7' in qsf
        assert 'Version 17.0.2 Build 602' in (project/'compile.log').read_text()
        decoded=(project/'top.bt').read_text()
        inv={line.split('\t')[0]:line.split('\t')[1:] for line in (project/'ce-inverters.tsv').read_text().splitlines()}
        for port,pin in PINS.items():
            assert inv[pin][1:]==['0','1'], pin
            assert f'set_location_assignment PIN_{pin} -to {port}' in qsf
            for key,value in [('FAST_OUTPUT_REGISTER','ON'),('D5_DELAY','0'),('CURRENT_STRENGTH_NEW','"16MA"'),('SLEW_RATE','1'),('BOARD_MODEL_FAR_C',str(load*1e-12))]:
                assert f'set_instance_assignment -name {key} {value} -to {port}' in qsf
            # Decode proves CE uses hardware control, not a data mux or tieoff.
            pin_lines=[line for line in decoded.splitlines() if line.endswith(' ; '+pin)]
            assert any(re.match(r'r .* GPIO\.[^:]+:CEOUT ; ',line) for line in pin_lines),pin
            assert any(':CE_OUTREG_TIEOFF_EN.' in line and ' 1 ; ' in line for line in pin_lines),pin
            assert any(':OUTREG_OUTPUT_SEL.' in line and ' SEL_SDR ; ' in line for line in pin_lines),pin
            assert not any(re.search(r':CEOUT 1 ; ',line) for line in pin_lines),pin
            for selector in ('RB_T9_SEL_OREG_DFF_DELAY','RB_T9_SEL_EREG_CFF_DELAY','USE_CLR_OUTREG_EN'):
                assert not any(':'+selector+'.' in line and ' 0 ; ' not in line for line in pin_lines),pin
        pad_summary=pads(evidence)
        assert set(pad_summary['bounds'])=={'write_data_early','write_data_late'}
        for key,bound in pad_summary['bounds'].items():
            assert bound['count']==156
            assert 0 <= bound['minimum']['value_ps'] <= bound['maximum']['value_ps'] <= 5280
        # Require every expected pad, corner, early/late and explicit transition.
        for corner in CORNERS:
            for kind in ('setup','hold'):
                for transition in ('rise','fall'):
                    rows=variant['reports'][f'{corner}/output-{transition}-{kind}.rpt']
                    assert {r['To Node'] for r in rows}==set(PINS)
        groups=collections.defaultdict(list)
        coverage=set()
        for report, rows in variant['reports'].items():
            if '/register-' not in report: continue
            kind='hold' if report.endswith('hold.rpt') else 'setup'
            for row in rows:
                node=row['To Node']
                if node not in {port+'~reg0' for port in PINS}: continue
                pts=row['points']; arrival=[p for p in pts if p['section']=='arrival' and p['stage']=='data']
                ingress=[p for p in arrival if p['type']=='IC' and p['node'].startswith(node+'|')]
                assert len(ingress)==1
                channel=ingress[0]['node'].split('|')[-1]
                assert channel in ('ena','d') and ingress[0]['location'].startswith('DDIOOUTCELL')
                data=[p for p in arrival if p['type']=='CELL' and p['node']==node]
                clock=[p for p in pts if p['section']=='required' and p['type']=='CELL' and p['node']==node]
                intrinsic=[p for p in pts if p['section']=='required' and p['node']==node and p['type']==('uTh' if kind=='hold' else 'uTsu')]
                assert len(data)==len(clock)==len(intrinsic)==1
                d,c,i=(p[0]['incremental_ns'] for p in (data,clock,intrinsic))
                value=round((i+(c-d if kind=='hold' else d-c))*1000,3)
                assert value <= (310 if kind=='hold' else 120)
                groups[channel+'_'+kind].append(value)
                coverage.add((report.split('/')[0],kind,channel,node))
        assert coverage=={(c,k,ch,p+'~reg0') for c in CORNERS for k in ('setup','hold') for ch in ('ena','d') for p in PINS}
        cs=clocks(project)
        for field,limit in [('period_ns',1540),('high_ns',790),('low_ns',770)]:
            b=cs['bounds']['DDIOOUTCELL_ingress_'+field]
            assert b['count']==52 and b['maximum']['required_ps']<=limit
        for corner in CORNERS:
            rows=(project/'clock-requirements'/corner/'requirements.tsv').read_text().splitlines()[1:]
            assert {r.split('\t')[0] for r in rows if r.split('\t')[1].startswith('DDIOOUTCELL')}=={p+'~reg0' for p in PINS}
        result[str(load)]={'pad_bounds':pad_summary['bounds'],
                          'fabric_bounds':{k:{'count':len(v),'maximum_ps':max(v)} for k,v in sorted(groups.items())},
                          'clock_bounds':{k:v for k,v in cs['bounds'].items() if k.startswith('DDIOOUTCELL_ingress')},
                          'files_sha256':variant['hashes']}
    assert (root/'reference0/top.v').read_bytes()==(root/'reference30/top.v').read_bytes()
    assert (root/'reference0/clocks.sdc').read_bytes()==(root/'reference30/clocks.sdc').read_bytes()
    norm=lambda p:[l for l in p.read_text().splitlines() if 'BOARD_MODEL_FAR_C' not in l]
    assert norm(root/'reference0/top.qsf')==norm(root/'reference30/top.qsf')
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    result=audit(args.root)
    if args.output: args.output.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: 13 actual pins, hardware CE, 0/30pF, four corners, pad transitions, data/CE checks and primitive clocks')
