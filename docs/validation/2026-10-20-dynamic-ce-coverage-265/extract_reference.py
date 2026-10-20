#!/usr/bin/env python3
"""Capture raw reference reports and file identities after both bounded fits."""
import sys,json,hashlib
from pathlib import Path
repo=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(repo/'mistral/tests/gpio-timing'))
from characterize import paths
from pad_summary import summarize
from clock_summary import summarize as clocks
root=Path(sys.argv[1])
for load in (0,30):
    p=root/f'reference{load}'
    reports={str(f.relative_to(p)):paths(f.read_text()) for f in sorted(p.glob('*_*mv_*c/*.rpt'))}
    hashes={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(p.rglob('*'))
            if f.is_file() and f.name not in ['evidence.json','pad-summary.json','clock-summary.json']}
    evidence=dict(device='5CSEBA6U23I7',io_standard='3.3-V LVTTL',output_load_pf=load,all_transitions=True,
                  variants={'dynamic-ce':dict(reports=reports,hashes=hashes)})
    (p/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    (p/'pad-summary.json').write_text(json.dumps(summarize(evidence),indent=2)+'\n')
    (p/'clock-summary.json').write_text(json.dumps(clocks(p),indent=2)+'\n')
