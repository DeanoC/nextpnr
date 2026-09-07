#!/usr/bin/env python3
"""Compare Quartus physical MLAB initialization with source logical INIT bits."""
import json
import re
from pathlib import Path
import sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path('.')
source=root.joinpath('top.v').read_text()
words={int(a):int(v,16) for a,v in re.findall(r"mem\[(\d+)\] = \d+'h([0-9a-f]+)",source)}
width=int(re.search(r'reg \[(\d+):0\]',source).group(1))+1
p=[0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18]
expected={}
for bit in range(width):
    logical=sum((words[a]>>bit&1)<<a for a in range(32))
    physical=sum((1-(logical>>a&1))<<p[31-a] for a in range(32))
    expected[bit]=[f'{logical:08x}',f'{physical:08x}']
observed=[]
for x,y,alm,high,low in re.findall(r's MLAB\.(\d+)\.(\d+):LUT_MASK\.(\d+) ([0-9a-f]{8})\.([0-9a-f]{8})',root.joinpath('top.bt').read_text()):
    for lane,mask in enumerate([low,high]):
        matches=[bit for bit,(_,expectedmask) in expected.items() if expectedmask==mask]
        if mask!='ffffffff' or matches:
            assert matches, (x,y,alm,lane,mask)
            observed.append(dict(x=int(x),y=int(y),alm=int(alm),lane=lane,mask=mask,matching_source_bits=matches))
covered={bit for row in observed for bit in row['matching_source_bits']}
assert covered==set(range(width)),(covered,width)
result=dict(formula='physical bit p[31-address]+32*lane = NOT INIT[address]',permutation=p,expected=expected,observed=observed)
root.joinpath('init-mapping.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'PASS: all {width} source INIT columns match Quartus masks; lanes {sorted({row["lane"] for row in observed})}')
