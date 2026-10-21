import json,collections,pathlib,sys
r=pathlib.Path(sys.argv[1]);out={}
for d in sorted(r.iterdir()):
 if d.name.startswith('rejected-') or not d.is_dir() or not (d/'timing.json').exists() or not (d/'routed.json').exists():continue
 t=json.loads((d/'timing.json').read_text());mod=json.loads((d/'routed.json').read_text())['modules']['top'];cells=mod['cells'];clocks=t['fmax'];summary={'fmax':clocks,'timing_summary':t['timing_summary'],'paths':[],'last_input_pins':{}}
 all_last=[]
 for name,c in cells.items():
  if c['type'] not in ['MISTRAL_ALUT2','MISTRAL_ALUT3','MISTRAL_ALUT4','MISTRAL_ALUT5']:continue
  width=int(c['type'][-1]);attr=c.get('attributes',{});raw=attr.get('FES_PINMAP_V1');port='ABCDE'[width-1]
  if raw:
   pins=json.loads(bytes.fromhex(raw))['pins'];all_last.extend(pins.get(port,[None,[]])[1:])
 summary['last_input_pins']=dict(collections.Counter(str(x) for x in all_last))
 for p in t['critical_paths']:
  if p['from'] not in ['posedge clk_sys','negedge clk_sys'] or p['to'] not in ['posedge clk_sys','negedge clk_sys']:continue
  segs=[]
  for s in p['path']:
   if s['type']!='logic':continue
   c=cells[s['from']['cell']];raw=c['attributes'].get('FES_PINMAP_V1');pins=json.loads(bytes.fromhex(raw))['pins'] if raw else {}
   segs.append({'cell':s['from']['cell'],'type':c['type'],'logical':s['from']['port'],'physical':pins.get(s['from']['port']),'delay_ns':s['delay']})
  summary['paths'].append({'from':p['from'],'to':p['to'],'endpoint':p['path'][-1]['to'],'slack_ns':p['max_delay']-sum(s['delay'] for s in p['path']),'logic_ns':sum(s['delay'] for s in p['path'] if s['type']=='logic'),'routing_ns':sum(s['delay'] for s in p['path'] if s['type']=='routing'),'logic':segs})
 out[d.name]=summary
(r/'analysis.json').write_text(json.dumps(out,indent=2));print(json.dumps({k:{'fmax':v['fmax'],'last_input_pins':v['last_input_pins'],'sys_paths':len(v['paths'])} for k,v in out.items()},indent=2))
