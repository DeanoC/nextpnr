#!/usr/bin/env python3
"""Portable audit of a rejected routed PC/XT capture and its waveform reference."""
import copy,hashlib,importlib.util,json,sys,tarfile,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
repo=next(p for p in here.parents if (p/'mistral/routed_swap.py').is_file())
sys.path.insert(0,str(repo/'mistral/tests/gpio-timing'))
from clock_waveform import evaluate
from analyse import analyse
manifest=json.loads((here/'manifest.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for name,digest in manifest['evidence_sha256'].items():assert sha(here/name)==digest,name
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp)
 with tarfile.open(here/'capture.tar.gz') as archive:archive.extractall(root,filter='data')
 for name,digest in manifest['capture_sha256'].items():assert sha(root/name)==digest,name
 actual=analyse(root);assert actual==json.loads((here/'result.json').read_text())
 assert actual['returncode']==1 and actual['final_analogue'] is True and actual['complete'] is False
 assert actual['setup_failure_rows']==489 and actual['related_hold_failure_rows']==0
 assert actual['remaining_route_seconds']==1474.3926381732292
 before=json.loads((root/'inputs/placed.json').read_text())
 after=json.loads((root/'inputs/diagnostic-placed.json').read_text())
 m=after['modules']['top'];decl=json.loads(m['settings'].pop('timing/forwarded_clocks'))
 assert decl==[dict(port='SDRAM_CLK',clock='clocks[0]',load_pf=30,uncertainty_ps=0)]
 attrs=m['cells']['SDRAM_CLK_MISTRAL_OB_PAD']['attributes']
 assert attrs.pop('NEXTPNR_GPIO_TIMING_PROFILE')=='QUARTUS_17_0_2_RAMTEST'
 assert attrs.pop('BOARD_MODEL_FAR_C')=='30P'
 assert after==before, 'unapproved input/configuration changes'
 assert 'timing/io_delays' not in m['settings'] and 'timing/allowFail' not in m['settings']
 pre=json.loads((root/'preflight.json').read_text())
 for name in ('placed.json','constraints.qsf','clocks.sdc'):
  assert sha(root/'inputs'/name)==pre['original_inputs'][name]['sha256']
 for name in ('diagnostic.rbf','endpoints.json','routed.json','timing.json'):
  assert sha(root/'route'/name)==json.loads((root/'route/receipt.json').read_text())['artifacts'][name]
 c=actual['waveform'];spread=c['route'][1]-c['route'][0]
 reference=json.loads((repo/'docs/validation/2026-10-21-clock-waveform-265/reference.json').read_text())
 pred=evaluate(reference,30,c['source_high'][0]-spread,c['source_low'][0]-spread,0)
 assert c['minimum_high']==pred['high_min_ps']==4154
 assert c['minimum_low']==pred['low_min_ps']==5064
 assert c['physically_qualified'] is False
 selectors=(root/'native-selectors.tsv').read_text()
 assert len(selectors.splitlines())==15
 for load in (0,15,30):assert selectors==(root/f'reference{load}-selectors.tsv').read_text()
 log=(root/'route/nextpnr.log').read_text()
 assert 'backend hip:' in log and 'FES-GPU-GRANT' in log and 'Router1 time' in log
 assert 'CPU reference' not in log
 spec=importlib.util.spec_from_file_location('guard',repo/'mistral/routed_swap.py')
 guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
 inventory=json.loads((root/'route/endpoints.json').read_text())
 assert not guard.compare(inventory,inventory)[0]
 print('PASS: frozen route/input hashes; only declared diagnostic changes; native pulse arithmetic; rejected timing remains rejected')
