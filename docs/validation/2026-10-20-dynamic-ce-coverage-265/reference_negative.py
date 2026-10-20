#!/usr/bin/env python3
"""Mutate independent evidence channels and require fail-closed reference audit."""
import copy,json,sys,tempfile,shutil
from pathlib import Path
from reference_check import audit,sha,CORNERS

def test(root):
    with tempfile.TemporaryDirectory() as temp:
        dest=Path(temp)
        for load in (0,30): shutil.copytree(root/f'reference{load}',dest/f'reference{load}')
        project=dest/'reference30'; original=json.loads((project/'evidence.json').read_text())
        cases=['missing_corner','missing_pad_transition','missing_ce_path','missing_data_path','missing_clock_check','disabled_hardware_ce','inverted_ce','nonzero_delay']
        for case in cases:
            evidence=copy.deepcopy(original);v=evidence['variants']['dynamic-ce']; restored={}
            def write(name,body):
                file=project/name;restored[name]=file.read_bytes();file.write_text(body);v['hashes'][name]=sha(file)
            corner=sorted(CORNERS)[0]
            if case=='missing_corner':
                v['reports']={k:r for k,r in v['reports'].items() if not k.startswith(corner+'/')}
            elif case=='missing_pad_transition':
                name=f'{corner}/output-rise-setup.rpt';write(name,'');v['reports'][name]=[]
            elif case in ('missing_ce_path','missing_data_path'):
                # Remove complete raw blocks and matching parsed paths so rejection
                # tests semantic coverage rather than just the digest binding.
                import re
                name=f'{corner}/register-setup.rpt';body=(project/name).read_text();channel='ena' if case=='missing_ce_path' else 'd'
                blocks=re.split(r'(?=^Path #\d+:)',body,flags=re.M)
                body=''.join(b for b in blocks if 'SDRAM_A[0]~reg0|'+channel+' ' not in b)
                write(name,body)
                from characterize import paths
                v['reports'][name]=paths(body)
            elif case=='missing_clock_check':
                name=f'clock-requirements/{corner}/pulse.rpt';write(name,'')
            elif case=='disabled_hardware_ce':
                name='top.bt';write(name,(project/name).read_text().replace(':CE_OUTREG_TIEOFF_EN.0 1 ; Y11',':CE_OUTREG_TIEOFF_EN.0 0 ; Y11'))
            elif case=='inverted_ce':
                name='ce-inverters.tsv';write(name,(project/name).read_text().replace('Y11\tGPIO.008.000.0:CEOUT\t0\t1','Y11\tGPIO.008.000.0:CEOUT\t1\t0'))
            else:
                name='top.bt';write(name,(project/name).read_text()+'s DQS16.004.000:RB_T9_SEL_OREG_DFF_DELAY.0 1 ; Y11\n')
            (project/'evidence.json').write_text(json.dumps(evidence))
            try: audit(dest)
            except (AssertionError,ValueError,KeyError): pass
            else: raise AssertionError('Accepted corrupt evidence: '+case)
            for name,body in restored.items(): (project/name).write_bytes(body)
            (project/'evidence.json').write_text(json.dumps(original))
            print('PASS rejection:',case)
if __name__=='__main__': test(Path(sys.argv[1]))
