import json,re
from pathlib import Path
root=Path('/home/deano/harnesses/pcxt-pad-models')
boundary={}
for line in (root/'boundaries.txt').read_text().splitlines():
 pin,port,_,node=line.split(); channel='data' if ':DATAOUT.' in port else 'oe' if ':OEIN.' in port else 'input'
 boundary[pin,channel]=node
pins=[line.split()[0] for line in (root/'boundaries.txt').read_text().splitlines()[::3]]
result=[]
for load, recipe in ((0,"dq-native0"),(30,"dq-native30"),(0,"dq-targeted0"),(30,"dq-targeted30")):
 for report in sorted((root/recipe).glob('*/*-*-*-*.rpt')):
  n,channel,edge,mode=report.stem.split('-');n=int(n);node=boundary[pins[n],channel]
  _,x,y,index=node.split('.');prefix='VIO_BUFFER' if channel=='input' else 'BLOCK_INPUT_MUX_PASSTHROUGH'
  needle=f'{prefix}_X{int(x)}_Y{int(y)}_N0_I{int(index)}'
  rows=[]
  for line in report.read_text().splitlines():
   cols=[v.strip() for v in line.split(';')]
   if len(cols)==9:
    try: rows.append((float(cols[1]),float(cols[2]),cols[6],cols[7]))
    except ValueError: pass
  match=[(i,r) for i,r in enumerate(rows) if r[2]==needle]
  exact=len(match)==1
  if not exact:
   match=[(i,r) for i,r in enumerate(rows) if r[2].startswith(prefix+'_')]
  assert len(match)==1,(report,needle,match)
  i,r=match[0]
  entry=dict(recipe=recipe,load_pf=load,pin=pins[n],channel=channel,edge=edge,mode=mode,corner=report.parent.name,node=node,expected_row=needle,actual_row=r[2],boundary_matches=exact)
  if channel=='input':
   entry.update(prefix_before_driver_ps=round((r[0]-r[1])*1000),prefix_after_driver_ps=round(r[0]*1000))
  else:
   assert rows[-1][2]=='PIN_'+pins[n],(report,rows[-1])
   entry['tail_ps']=round((rows[-1][0]-r[0])*1000)
  result.append(entry)
assert len(result)==3072,len(result)
Path('/tmp/pcxt-pad-reference-audit.json').write_text(json.dumps(result,indent=2)+'\n')
for c in ('data','oe','input'):
 r=[x for x in result if x['channel']==c];field='prefix_before_driver_ps' if c=='input' else 'tail_ps'
 print(c,len(r),min(x[field] for x in r),max(x[field] for x in r),'boundary mismatches',len([x for x in r if not x['boundary_matches']]))
