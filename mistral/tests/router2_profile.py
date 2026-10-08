#!/usr/bin/env python3
"""Verify profiling preserves routes and observer paths cannot leak across reloads."""
import argparse
import json
from pathlib import Path
import subprocess
from router2_undriven import fixture

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--nextpnr',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
placed=out/'placed.json';placed.write_text(json.dumps(fixture(True,True)))

def route(name,source,profile=None):
    routed=out/(name+'.json')
    command=[str(args.nextpnr.resolve()),'--json',str(source),'--device','5CSEBA6U23I7',
        '--router','router2','--no-pack','--no-place','--timing-allow-fail','--write',str(routed)]
    if profile is not None:command+=['--router2-profile',str(profile)]
    with (out/(name+'.log')).open('w') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
    return json.loads(routed.read_text())['modules']['top']

plain=route('plain',placed)
old=out/'old-profile.json';observed=route('observed',placed,old)
def logical_snapshot(module):
    # JSON numeric IDs depend on the number of interned setting strings.
    # Compare named connectivity and exact physical route trees instead.
    bits={bit:(name,index) for name,net in module['netnames'].items() for index,bit in enumerate(net['bits'])}
    for cell in module['cells'].values():
        cell['connections']={name:[bits[bit] if type(bit) is int else bit for bit in values]
                             for name,values in cell['connections'].items()}
    for name,net in module['netnames'].items():
        net['bits']=[bits[bit] if type(bit) is int else bit for bit in net['bits']]
    return module['cells'],module['netnames']
assert logical_snapshot(plain)==logical_snapshot(observed)
snapshot=json.loads(old.read_text())
assert snapshot['completed'] and snapshot['calls_started']>=1 and snapshot['calls_started']==snapshot['calls_finished']
old_bytes=old.read_bytes()
# Keep the placed step and insert a stale observer setting, to force a real route.
loaded=json.loads(placed.read_text());loaded['modules']['top']['settings']['router2/profilePath']=str(old)
stale=out/'stale.json';stale.write_text(json.dumps(loaded))
without=route('without',stale)
assert old.read_bytes()==old_bytes and 'router2/profilePath' not in without['settings']
new=out/'new-profile.json';replaced=route('replaced',stale,new)
assert old.read_bytes()==old_bytes and json.loads(new.read_text())['completed']
assert replaced['settings']['router2/profilePath']==str(new)
print('Router2 native profile equivalence and checkpoint observer-path isolation passed')
