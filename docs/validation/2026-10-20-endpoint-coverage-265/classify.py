#!/usr/bin/env python3
"""Read checkpoint connectivity; never infer timing from unchanged artifacts."""
import collections, json, re, sys
from pathlib import Path
root=Path(sys.argv[1]); snapshot=json.loads((root/'endpoints.json').read_text())
module=next(iter(json.loads((root/'routed.json').read_text())['modules'].values()))
cells=module['cells']; drivers={}
for name, cell in cells.items():
    for port,bits in cell['connections'].items():
        if cell['port_directions'][port]=='output':
            for bit in bits: drivers[bit]=(name,port)
def source(cell,port):
    m=re.fullmatch(r'(.*)\[(\d+)\]',port)
    bit=cells[cell]['connections'][m[1]][int(m[2])] if m else cells[cell]['connections'][port][0]
    return drivers.get(bit)
def trace(pair,seen=()):
    if pair is None:return {'kind':'undriven'}
    name,port=pair; cell=cells[name]
    if name in seen:return {'kind':'cycle','cell':name}
    result={'cell':name,'type':cell['type'],'port':port}
    if cell['type']=='MISTRAL_CONST':result['params']=cell['parameters']
    else:
        result['inputs']={p:trace(source(name,p),seen+(name,)) for p in cell['connections']
                          if cell['port_directions'][p]=='input'} if cell['type'] in ('MISTRAL_BUF','MISTRAL_ALUT2') else {}
    return result
counts=collections.Counter(); details=[]
for name,port in snapshot['unsupported']:
    tree=trace(source(name,port));counts[(cells[name]['type'],tree.get('type','undriven'))]+=1
    details.append({'endpoint':[name,port],'source':tree})
print(json.dumps({'unsupported':len(details),'by_sink_and_direct_driver':{str(k):v for k,v in sorted(counts.items())},
                  'details':details},indent=2,sort_keys=True))
