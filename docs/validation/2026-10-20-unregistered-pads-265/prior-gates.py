#!/usr/bin/env python3
"""Run immutable prior evidence gates, adapting only their isolated host fixtures.
No historical whole-design inventory is promoted to a current schema.
"""
from pathlib import Path
import sys
repo=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve();out.mkdir(exist_ok=True)
names=('critical-swaps','routed-swap-guard','endpoint-coverage','io-reset','output-coverage','registered-pad-coverage','dynamic-ce-coverage','interface-contracts')
import subprocess
for name in names:
 p=repo/f'docs/validation/2026-10-20-{name}-265/check.py'
 code=p.read_text().replace('schema=6,','schema=7,unregistered_contracts={},')
 if name=='interface-contracts':
  code=code.replace("fixtures=read(here/'constant-fixtures.json.gz')", "fixtures=read(here/'constant-fixtures.json.gz')\n# Adapt only the explicitly isolated constant host fixtures.\nfor fixture in fixtures.values():\n assert fixture['schema']==6\n fixture.update(schema=7,unregistered_contracts={})")
 runner="import sys\nsys.argv=["+repr(str(p))+"]\n__file__="+repr(str(p))+"\n"+code
 r=subprocess.run([sys.executable,'-c',runner],cwd=repo,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (out/(name+'.log')).write_text(r.stdout);print(name,r.returncode)
 if r.returncode:print(r.stdout[-1500:]);raise SystemExit(r.returncode)
