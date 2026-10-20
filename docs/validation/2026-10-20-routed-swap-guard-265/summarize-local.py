import hashlib,json
from pathlib import Path
r=Path('paired');old=Path('/home/deano/harnesses/pcxt-compiler-repair/nextpnr/experiments')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
flatten=lambda s:{(ep,tuple(row['launch']),tuple(row['capture'])):row for ep,rows in s['endpoints'].items() for row in rows}
b=json.loads((r/'baseline/endpoints.json').read_text());c=json.loads((r/'candidate/endpoints.json').read_text())
bf,cf=flatten(b),flatten(c);keys=bf.keys()&cf.keys()
reg=[];newfail=[];hold=[]
for key in sorted(keys):
 a,z=bf[key],cf[key]
 item={'endpoint':json.loads(key[0]),'launch':key[1],'capture':key[2],'baseline_setup_ps':a['setup'],'candidate_setup_ps':z['setup'],'baseline_hold_ps':a['hold'],'candidate_hold_ps':z['hold']}
 if a['setup_timed'] and z['setup_timed'] and z['setup']<a['setup']:reg.append(item)
 if a['setup_timed'] and z['setup_timed'] and a['setup']>=0>z['setup']:newfail.append(item)
 if a['hold_related'] and z['hold_related'] and z['hold']<a['hold']:hold.append(item)
report={'scope':'DIAGNOSTIC intersection of available native rows only. Missing coverage forbids acceptance.',
'baseline':{'complete':b['complete'],'endpoints':len(b['endpoints']),'pairs':len(bf),'unsupported':len(b['unsupported'])},
'candidate':{'complete':c['complete'],'endpoints':len(c['endpoints']),'pairs':len(cf),'unsupported':len(c['unsupported'])},
'common_pairs':len(keys),'baseline_only_pairs':len(bf.keys()-cf.keys()),'candidate_only_pairs':len(cf.keys()-bf.keys()),
'setup_regressions':len(reg),'newly_failing_setup':len(newfail),'hold_regressions':len(hold),'newly_failing_examples':newfail[:25],
'prior_baseline_rbf_equal':sha(r/'baseline/diagnostic.rbf')==sha(old/'baseline-expanded-route/diagnostic.rbf'),
'prior_candidate_rbf_equal':sha(r/'candidate/diagnostic.rbf')==sha(old/'candidate-expanded-route/diagnostic.rbf'),
'exact_retention':{name:sha(r/'retained'/name)==sha(r/'baseline'/name) for name in ['diagnostic.rbf','routed.json','timing.json','endpoints.json']},
'baseline_timing':json.loads((r/'baseline/timing.json').read_text())['timing_summary'],
'candidate_timing':json.loads((r/'candidate/timing.json').read_text())['timing_summary'],
'baseline_fmax':json.loads((r/'baseline/timing.json').read_text())['fmax'],
'candidate_fmax':json.loads((r/'candidate/timing.json').read_text())['fmax'],
'prior_baseline_clock_summary_equal':json.loads((r/'baseline/timing.json').read_text())['timing_summary']==json.loads((old/'baseline-expanded-route/timing.json').read_text())['timing_summary'],
'prior_candidate_clock_summary_equal':json.loads((r/'candidate/timing.json').read_text())['timing_summary']==json.loads((old/'candidate-expanded-route/timing.json').read_text())['timing_summary'],
'decision':json.loads((r/'decision.json').read_text())}
Path('comparison.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['newly_failing_examples','decision']},indent=2))
