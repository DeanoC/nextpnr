import json,collections
from pathlib import Path
m=json.load(open('/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json'))['modules']['top'];cs=m['cells'];dr={b:(n,p) for n,c in cs.items() for p,v in c['connections'].items() if c.get('port_directions',{}).get(p)=='output' for b in v};users=collections.defaultdict(list)
for n,c in cs.items():
 for p,v in c['connections'].items():
  if c.get('port_directions',{}).get(p)=='input':
   for b in v:users[b].append((n,p))
a=json.load(open('/tmp/pcxt-reset-intent-audit.json'));result=[]
for clock,chain in a['graphs']['placed']['chains'].items():
 final=chain['stages'][-1];pending=list(cs[final]['connections']['Q']);seen=set();leaves=[]
 while pending:
  b=pending.pop()
  if b in seen:continue
  seen.add(b)
  for n,p in users[b]:
   c=cs[n]
   if c['type'].startswith('MISTRAL_ALUT') or c['type'] in ('MISTRAL_BUF','MISTRAL_NOT'):
    pending.extend(c['connections'].get('Q',[]))
   else:leaves.append({'cell':n,'port':p,'type':c['type'],'clock':[dr.get(x) for x in c['connections'].get('CLK',[])]})
 count=collections.Counter((x['type'],x['port'],str(x['clock'])) for x in leaves)
 result.append({'chain_clock':clock,'final':final,'combinational_nets':len(seen),'leaf_count':len(leaves),'leaves':leaves})
 print(clock,'nets',len(seen),'leaf_count',len(leaves),'groups',dict(count))
Path('/tmp/pcxt-reset-fanout-audit.json').write_text(json.dumps(result,indent=2)+'\n')
