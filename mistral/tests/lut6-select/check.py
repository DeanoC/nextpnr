import collections,csv,json,re,sys
from pathlib import Path
p=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent/'asymmetric'
bt=(p/'top.bt').read_text();qsf=(p/'top.qsf').read_text()
rows=list(csv.DictReader((p/'arcs-7_slow_1100mv_100c.tsv').open(),delimiter='\t'))
cell=next(r for r in rows if r['cell']=='lut6');x,y,z=map(int,re.fullmatch(r'LABCELL_X(\d+)_Y(\d+)_N(\d+)',cell['location']).groups());alm=z//6;lane=1 if z%6==3 else 0
prefix=f'LAB.{x:03d}.{y:03d}.{alm}:'
graph=collections.defaultdict(set)
for line in bt.splitlines():
 parts=line.split()
 if len(parts)==3 and parts[0]=='r':graph[parts[1]].add(parts[2])
mapping={};routes={}
for q in range(6):
 m=re.search(r'set_location_assignment FF_X(\d+)_Y(\d+)_N(\d+) -to q\['+str(q)+r'\]',qsf)
 if not m:raise ValueError('missing register location')
 fx,fy,fz=map(int,m.groups());port={1:'FFT0',2:'FFT1',4:'FFB0',5:'FFB1'}[fz%6];fbase=f'LAB.{fx:03d}.{fy:03d}.{fz//6}:'
 queue=collections.deque([(fbase+port,[fbase+port]),(fbase+port+'L',[fbase+port+'L'])]);seen=set();hits=[]
 while queue:
  node,path=queue.popleft()
  if node in seen:continue
  seen.add(node)
  if node.startswith(prefix) and node[len(prefix):] in ['A','B','C','D',f'E{lane}',f'F{lane}']:
   hits.append(path);continue
  for nxt in sorted(graph[node]):queue.append((nxt,path+[nxt]))
 if len(hits)!=1:raise ValueError(f'q{q} has {len(hits)} target inputs: {hits}')
 mapping[q]=hits[0][-1].split(':')[-1];routes[q]=hits[0]
m=re.search(r's LAB\.%03d\.%03d:LUT_MASK\.%d ([0-9a-f]+)\.([0-9a-f]+)'%(x,y,alm),bt)
actual=int(m[1]+m[2],16)
masks=re.findall(r"lut_mask\(64'h([0-9a-fA-F]+)\).*?\) lut(\d+)", (p/'top.v').read_text())
init=int(next(mask for mask,name in masks if name=='6'),16)
results=[]
for swap_ef in [False,True]:
 for swap_cd in [False,True]:
  mask=0
  for bit in range(64):
   vals={'A':bit&1,'B':(bit>>1)&1,'C':(bit>>(3 if bit>=32 else 2))&1,'D':(bit>>(2 if bit>=32 else 3))&1,'E0':(bit>>5)&1,'F0':(bit>>4)&1}
   if swap_ef:vals['E0'],vals['F0']=vals['F0'],vals['E0']
   if swap_cd:vals['C'],vals['D']=vals['D'],vals['C']
   vals['E1'],vals['F1']=vals['E0'],vals['F0']
   index=sum((1-vals[mapping[q]])<<q for q in range(6))
   mask|=(1-((init>>index)&1))<<bit
  results.append({'swap_ef':swap_ef,'swap_cd':swap_cd,'predicted_mask':f'{mask:016x}','mismatching_bits':(mask^actual).bit_count()})
corner_delays={}
for report in sorted(p.glob('arcs-*.tsv')):
 delays={}
 with report.open() as stream:
  for row in csv.DictReader(stream,delimiter='\t'):
   if row['cell']!='lut6':continue
   q=int(re.fullmatch(r'q\[(\d+)\]',row['launch'])[1])
   delays[mapping[q]]=max(float(row[k]) for k in ['rr_max','rf_max','fr_max','ff_max'] if row[k])
 assert set(delays)==set(mapping.values()), 'incomplete physical input arc coverage'
 assert delays[f'E{lane}']>delays[f'F{lane}'], 'physical F is not the faster input'
 corner_delays[report.stem]=delays
assert len(corner_delays)==4, 'four operating corners are required'
result={'quartus_mask':f'{actual:016x}','source_init':f'{init:016x}','cell_location':cell['location'],'corner_max_delays_ns':corner_delays,'source_register_to_mistral_physical_input':mapping,'decoded_routes':routes,'predictions':results,'cell_delays':[{k:r[k] for k in ['launch','input','rr_max','rf_max','fr_max','ff_max']} for r in rows if r['cell']=='lut6']}
print(json.dumps(result,indent=2))

assert results[0]["mismatching_bits"] == 0, "nextpnr truth convention differs from Quartus"
assert all(r["mismatching_bits"] > 0 for r in results[1:]), "fixture does not distinguish the alternate mappings"
