import json,sys
x=json.load(open(sys.argv[1]));a=x['reset'];b=x['unregistered']
assert set(a['endpoints'])==set(b['endpoints']) and len(b['endpoints'])==17246
for k in ('clocks','io_constraints','primitive_clocks','registered_contracts','reset_contracts','forwarded_clocks','constant_outputs','static_endpoints','unsupported','unsupported_details'):
 assert a[k]==b[k],k
assert len(b['static_endpoints'])==1888
changed=[]
for k,old in a['endpoints'].items():
 new=b['endpoints'][k]
 assert len(old)==len(new)
 if old==new:continue
 changed.append(k)
 for o,n in zip(old,new):
  for f in o:
   if f not in ('max_path','setup'):assert o[f]==n[f],(k,f)
  delta=n['max_path']-o['max_path']
  assert delta>0 and o['setup']-n['setup']==delta,(k,o,n)
assert len(changed)==48
assert len(b['unregistered_contracts'])==48
assert len(b['physical_obligations'])==52
assert not b['complete']
print('PASS: 17246 rows,1888 proofs;48 endpoints changed only by positive max-path/setup deltas; holds, clocks, constraints and prior contracts unchanged; remains incomplete.')
