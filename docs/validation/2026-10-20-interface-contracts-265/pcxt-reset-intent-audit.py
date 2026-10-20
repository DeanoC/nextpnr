import json,hashlib,subprocess,collections
from pathlib import Path
fes=Path('/home/deano/harnesses/worktrees/fes/silver-delta/out/dev/pcxt-board')
r=fes/'sources/misteross/build/diagnostic/fes-pcxt/reset-metadata-20261020'
paths={'placed':Path('/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json'),'corrected_packed':r/'corrected-packed.json','corrected':r/'corrected.json','baseline':r/'baseline.json'}
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
receipt=json.loads((r/'packed-metadata-result.json').read_text())
for k in ('baseline','corrected','corrected_packed'): assert sha(paths[k])==receipt['artifacts'][paths[k].name]
rtl='sources/misteross/cores/fes-pcxt/rtl/fes_pcxt_top.sv';rtlb=subprocess.check_output(['git','show','479cf1da:'+rtl],cwd=fes);assert rtlb==(fes/rtl).read_bytes()
a=json.loads(paths['baseline'].read_text());b=json.loads(paths['corrected'].read_text())
for c in b['modules']['top']['cells'].values():c['attributes'].pop('async_reg',None)
assert a==b
models={k:json.loads(p.read_text())['modules']['top'] for k,p in paths.items() if k in ('placed','corrected_packed')}
records={}
for kind,m in models.items():
 cs=m['cells'];dr={bit:(name,port) for name,c in cs.items() for port,bits in c['connections'].items() if c.get('port_directions',{}).get(port)=='output' for bit in bits}
 users=collections.defaultdict(list)
 for name,c in cs.items():
  for port,bits in c['connections'].items():
   if c.get('port_directions',{}).get(port)=='input':
    for bit in bits:users[bit].append((name,port))
 reset=cs['locked_MISTRAL_ALUT2_Q'];resetbit=reset['connections']['Q'][0]
 stages={name:c for name,c in cs.items() if c['type']=='MISTRAL_FF' and c['connections'].get('ACLR')==[resetbit]}
 assert len(stages)==4
 chains={}
 for name,c in stages.items():
  di=dr[c['connections']['DATAIN'][0]]
  if cs[di[0]]['type']!='MISTRAL_CONST':continue
  assert int(cs[di[0]]['parameters']['LUT'],2)==1
  clk=dr[c['connections']['CLK'][0]]
  q=c['connections']['Q'][0];sinks=users[q];assert len(sinks)==1 and sinks[0][1]=='DATAIN'
  second=sinks[0][0];assert second in stages
  members=[name,second]
  for n in members:
   sc=stages[n];assert dr[sc['connections']['CLK'][0]]==clk
   assert not any(sc['connections'].get(p) for p in ('SCLR','SLOAD','SDATA','ENA'))
   if kind=='corrected_packed':assert sc['attributes']['async_reg']=='true'
  chains[clk[0]]={'stages':members,'source_cells':{n:stages[n] for n in members},'final_consumers':users[stages[second]['connections']['Q'][0]]}
 assert len(chains)==2
 records[kind]={'chains':chains,'reset_cell':reset,'reset_fanout':users[resetbit]}
assert records['placed']['chains'].keys()==records['corrected_packed']['chains'].keys()
# Independent stage identity is clock domain and position in verified two-stage chain, never name coincidence.
mapping=[]
for clk,x in records['placed']['chains'].items():
 y=records['corrected_packed']['chains'][clk]
 for i,(old,new) in enumerate(zip(x['stages'],y['stages'])):
  mapping.append({'placed_cell':old,'intent_source_cell':new,'clock_buffer':clk,'stage_index':i,'async_reg':y['source_cells'][new]['attributes']['async_reg']})
out={'classification':'audited reset intent correspondence; not physical qualification or whole-design equivalence','fes_commit':'479cf1dafb9da7953325571084515e91f6d27f7e','rtl_sha256':hashlib.sha256(rtlb).hexdigest(),'source_sha256':{k:sha(p) for k,p in paths.items()},'source_paths':{k:str(p) for k,p in paths.items()},'metadata_only_synthesis_comparison':True,'mapping':mapping,'graphs':records}
Path('/tmp/pcxt-reset-intent-audit.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({'mapping':mapping,'source_sha256':out['source_sha256']},indent=2))
