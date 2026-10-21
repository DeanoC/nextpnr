import csv,json,re
from pathlib import Path
r=Path('/tmp/issue236-closure');rep=r/'full-replica'
p=json.load(open(r/'clock-aware-s1/timing.json'))['critical_paths'][0]
manifest=json.load(open(rep/'replica.json'))
orig={row['dst']:row for row in csv.DictReader(open(r/'full-routes.tsv'),delimiter='\t')}
routes={row['dst']:row for row in csv.DictReader(open(rep/'routes.tsv'),delimiter='\t')}
dsts=[]
for l in manifest['luts']:
 if l['phys']=='CI':continue
 dst=f"{'MLAB' if l['kind']=='MISTRAL_MCOMB' else 'LAB'}.{l['x']:03d}.{l['y']:03d}.{l['alm']}:{l['phys']}"
 assert routes[dst]['chain']==orig[dst]['chain'],(dst,routes[dst]['chain'],orig[dst]['chain'])
 dsts.append(dst)
print('EXACT_ROUTE_CHAINS',len(dsts))
cols={'100c':('rise100','fall100'),'-40c':('risen40','falln40')}
def delay(row,corner,port=True):
 a,b=cols[corner];prefix='port_' if port else '';return max(float(row[prefix+a]),float(row[prefix+b]))
last=p['path'][-2];last_ps=1000*last['delay'];finals=[row for row in orig.values() if re.match(r'LAB\.011\.008[.:]',row['dst']) and min(abs(delay(row,'100c')-last_ps),abs(delay(row,'100c',False)-last_ps))<1.5]
assert len(finals)==1,[(row['src'],row['dst']) for row in finals]
final=finals[0]
np_data=sum(l['route_ps']+l['cell_ps'] for l in manifest['luts']);total=sum(s['delay']*1000 for s in p['path'])
results={'exact_routed_hops':len(dsts),'dedicated_carry_hops':3,'report_ps':round(total),'report_mhz':1e6/total,'data_nextpnr_ps':round(np_data),'final_sink':final['dst']}
for corner in cols:
 rows=list(csv.DictReader(open(rep/f'paths-7_slow_1100mv_{corner}.tsv'),delimiter='\t'))
 first=next(row['path'] for row in rows if row['src'].startswith('q0'))
 pts=[row for row in rows if row['path']==first and row['point'].startswith('arrival')]
 cells=[row for row in pts if row['type']=='cell' and row['src_type']=='cyclonev_lcell_comb']
 assert len(cells)==14,len(cells)
 for i,(row,l) in enumerate(zip(cells,manifest['luts']),1):
  assert row['src'].startswith(f'l{i}|'),row
  if l['phys']=='CI': assert row['src_port']=='cin',row
  expected=f"{'MLAB' if l['kind']=='MISTRAL_MCOMB' else 'LAB'}CELL_X{l['x']}_Y{l['y']}_N{6*l['alm']+3*l['half']}"
  assert row['src_loc']==expected,(i,row['src_loc'],expected)
  assert row['dst_port']=={'Q':'combout','CO':'cout','SO':'sumout'}[l['out_port']],row
 q=sum(float(row['incr'])*1000 for row in pts if (row['type']=='ic' and row['dst_type']=='cyclonev_lcell_comb') or (row['type']=='cell' and row['src_type']=='cyclonev_lcell_comb'))
 side=sum(delay(orig[d],corner)-delay(routes[d],corner) for d in dsts)
 full=total-np_data-last_ps+q+side+delay(final,corner)
 results[corner]={'data_quartus_ps':round(q),'side_branch_ps':round(side),'final_hop_nextpnr_ps':round(last_ps),'final_hop_ps':round(delay(final,corner)),'full_ps':round(full),'mhz':1e6/full}
print(json.dumps(results,indent=2))
(r/'full-comparison.json').write_text(json.dumps(results,indent=2))
