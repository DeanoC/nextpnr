#!/usr/bin/env python3
"""Summarize one final routed diagnostic; never converts it to acceptance."""
import collections,json
from pathlib import Path

def analyse(root):
 root=Path(root);e=json.loads((root/'route/endpoints.json').read_text());t=json.loads((root/'route/timing.json').read_text());r=json.loads((root/'route/receipt.json').read_text())
 rows=[dict(endpoint=k,**v) for k,vs in e['endpoints'].items() for v in vs]
 setup=[v for v in rows if v.get('setup') is not None];hold=[v for v in rows if v.get('hold') is not None]
 paths=[]
 for key in ['vram.7.0.0','eu_alu_last_result_MISTRAL_FF_Q_5']:
  v=next(v for v in t['critical_paths'] if v['path'] and key in v['path'][-1]['to']['cell']);sums=collections.defaultdict(float)
  for segment in v['path']:sums[segment['type']]+=segment['delay']
  paths.append(dict(endpoint=v['path'][-1]['to'],launch=v['path'][0]['from'],delay_by_type_ns=dict(sums)))
 c=e['forwarded_clocks']['SDRAM_CLK'];log=(root/'route/nextpnr.log').read_text()
 return dict(returncode=r['returncode'],compiler_commit=r['compiler_commit'],final_analogue=e['final_analogue'],complete=e['complete'],endpoint_count=len(e['endpoints']),unsupported_count=len(e['unsupported']),setup_failure_rows=sum(v['setup']<0 for v in setup),related_hold_failure_rows=sum(v['hold']<0 for v in hold),minimum_related_hold_ps=min(v['hold'] for v in hold),worst_setup_rows=sorted(setup,key=lambda v:v['setup'])[:5],fmax=t['fmax'],timing_summary=t['timing_summary'],waveform={k:c[k] for k in ['route','source_high','source_low','declaration','local_high_loss','local_low_loss','minimum_high','minimum_low','physically_qualified']},critical_paths=paths,router1_fallback='Router1 time' in log,charged_wall_seconds=r['charged_wall_seconds'],remaining_route_seconds=r['remaining_route_seconds_after'],physical_signoff=False,hardware_used=False)

if __name__=='__main__':
 import sys
 print(json.dumps(analyse(sys.argv[1]),indent=2))
