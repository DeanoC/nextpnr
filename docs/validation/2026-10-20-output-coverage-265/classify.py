#!/usr/bin/env python3
"""Account for every frozen physical output and remaining unsupported endpoint."""
import json
from pathlib import Path
from collections import Counter
ROOT=Path(__file__).resolve().parent
fixture=Path('/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json')
m=next(iter(json.loads(fixture.read_text())['modules'].values()))
drivers={}
for name,cell in m['cells'].items():
 for port,bits in cell['connections'].items():
  if cell['port_directions'].get(port)=='output':
   for b in bits: drivers[b]=(name,port)
def source(bit,seen=()):
 if isinstance(bit,str):return {'literal':bit}
 if bit in seen:return {'cycle':bit}
 if bit not in drivers:return {'undriven':bit}
 n,p=drivers[bit];c=m['cells'][n]
 result={'cell':n,'type':c['type'],'port':p,'params':c['parameters']}
 if c['type']=='MISTRAL_BUF':result['upstream']=[source(b,seen+(bit,)) for b in c['connections']['A']]
 if c['type'].startswith('MISTRAL_ALUT') and len(seen)<2:result['input_sources']={pin:[source(b,seen+(bit,)) for b in bits] for pin,bits in c['connections'].items() if c['port_directions'].get(pin)=='input'}
 return result
s=json.loads((ROOT/'pcxt-host-diagnostic.json').read_text());a,b=s['original'],s['synthetic']
old=json.loads(Path('/home/deano/harnesses/pcxt-io-reset/pcxt-host-diagnostic.json').read_text())
summary={n:{'dynamic_pairs':sum(map(len,v['endpoints'].values())),'static_proofs':len(v['static_endpoints']),'unsupported':len(v['unsupported']),'complete':v['complete'],'final_analogue':v['final_analogue']} for n,v in s.items()}
summary['old_original_rows_identical']=a['endpoints']==old['original']['endpoints']
summary['old_input_rows_identical']=all(b['endpoints'].get(k)==v for k,v in old['synthetic']['endpoints'].items())
summary['old_static_proofs_identical']=a['static_endpoints']==b['static_endpoints']==old['original']['static_endpoints']
summary['old_clocks_identical']=a['clocks']==b['clocks']==old['original']['clocks']
remaining=[]
for n,p in b['unsupported']:
 c=m['cells'][n]
 kind=('reset-aclr' if p=='ACLR' else 'registered-sdram-pad' if p=='PAD' else 'hdmi-clock-forwarder' if n.startswith('HDMI_TX_CLK') else 'hps-i2c-constant-data' if p=='I' else 'hps-i2c-enable-unmodeled-source')
 remaining.append({'cell':n,'port':p,'class':kind,'type':c['type'],'params':c['parameters'],'attributes':c['attributes'],'source':[source(bit) for bit in c['connections'].get(p,[])], 'input_sources':{pin:[source(bit) for bit in bits] for pin,bits in c['connections'].items() if c['port_directions'].get(pin)=='input'}})
summary['remaining_classes']=dict(Counter(r['class'] for r in remaining))
summary['resolved_from_input_only']=sorted(set(map(tuple,old['synthetic']['unsupported']))-set(map(tuple,b['unsupported'])))
summary['newly_exposed_pad_boundaries']=sorted(set(map(tuple,b['unsupported']))-set(map(tuple,old['synthetic']['unsupported'])))
summary['resolved_output_classes']=dict(Counter('sdram-data' if n.startswith('SDRAM_DQ_MISTRAL') and p=='I' else 'sdram-oe' if p=='OE' else 'sdram-mask' if n.startswith('SDRAM_DQM') else 'hdmi-data-control' for n,p in summary['resolved_from_input_only']))
# Account for all output pad nets, even where ordinary timing ignores PAD.
physical=[]
for n,c in m['cells'].items():
 if c['type'] not in ('MISTRAL_OB','MISTRAL_IO','MISTRAL_SDROUT','MISTRAL_SDRIO','MISTRAL_DDROUT','MISTRAL_DDRBIDIR'):continue
 ports=[]
 for top,info in m['ports'].items():
  if info['direction']=='input':continue
  for i,bit in enumerate(info['bits']):
   if bit in c['connections'].get('PAD',[]):ports.append(top if len(info['bits'])==1 else f'{top}[{i}]')
 keys=[k for k in b['endpoints'] if json.loads(k)[0]==n]
 physical.append({'cell':n,'type':c['type'],'top_ports':ports,'dynamic_endpoints':[json.loads(k)[1] for k in keys],'unsupported':[p for cell,p in b['unsupported'] if cell==n]})
(ROOT/'host-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(ROOT/'remaining-endpoints.json').write_text(json.dumps(remaining,indent=2)+'\n')
(ROOT/'physical-output-accounting.json').write_text(json.dumps(physical,indent=2)+'\n')
print(json.dumps(summary,indent=2))
