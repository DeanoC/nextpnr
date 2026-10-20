#!/usr/bin/env python3
"""Exact-boundary reference extraction; unmatched GINs never qualify."""
import json,re,hashlib,argparse
from pathlib import Path
CORNERS=('7_slow_1100mv_-40c','7_slow_1100mv_100c','MIN_fast_1100mv_-40c','MIN_fast_1100mv_100c')
def extract(root):
 root=Path(root); boundaries=[line.split() for line in (root/'boundaries.txt').read_text().splitlines()]
 assert len(boundaries)==48
 defaults=(root/'defaults.tsv').read_text().splitlines()
 rows=[]; rejected=[]; hashes={}
 for project,load in [('dq-native0',0),('dq-native30',30),('dq-targeted0',0),('dq-targeted30',30)]:
  p=root/project
  config=(p/'config.tsv').read_text().splitlines();assert len(config)==len(defaults)==128
  for d,c in zip(defaults,config):
   d=d.split();c=c.split();assert d[:3]==c[:3]
   if c[1]=='RB_T9_SEL_EREG_CFF_DELAY':assert c[4:]==['1f']
   else: assert c[4:]==d[4:] and c[3]=='1'
  qsf=(p/'top.qsf').read_text()
  assert 'invalid node name' not in (p/'compile.log').read_text()
  for i in range(16):
   pin=boundaries[3*i][0]
   assert f'set_location_assignment PIN_{pin} -to dq[{i}]' in qsf
   for k,v in [('D1_DELAY','0'),('D3_DELAY','0'),('D5_DELAY','0'),('D5_OE_DELAY','31'),('FAST_INPUT_REGISTER','OFF'),('FAST_OUTPUT_REGISTER','OFF'),('FAST_OUTPUT_ENABLE_REGISTER','OFF')]:
    assert f'set_instance_assignment -name {k} {v} -to dq[{i}]' in qsf
   for ch,j in [('data',0),('oe',1),('input',2)]:
    native=boundaries[3*i+j][-1]; xyz=tuple(map(int,native.split('.')[1:]))
    expected=('VIO_BUFFER_X%d_Y%d_N0_I%d' if ch=='input' else 'BLOCK_INPUT_MUX_PASSTHROUGH_X%d_Y%d_N0_I%d')%xyz
    for corner in CORNERS:
     for edge in ('rise','fall'):
      for mode in ('early','late'):
       path=p/corner/f'{i}-{ch}-{edge}-{mode}.rpt';text=path.read_text()
       hashes[str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
       assert 'Found 1 paths.' in text
       data=[]
       for line in text.splitlines():
        cols=[x.strip() for x in line.split(';')]
        if len(cols)==9 and re.fullmatch(r'-?\d+\.\d+',cols[1]) and cols[6]:data.append(cols)
       assert data
       ps=lambda s:round(float(s)*1000)
       for a,b in zip(data,data[1:]):assert abs(ps(a[1])+ps(b[2])-ps(b[1]))<=2
       matches=[r for r in data if r[6]==expected]
       if not matches:
        assert ch=='input' and project.startswith('dq-native')
        rejected.append(dict(pin=pin,load_pf=load,boundary=native,report=str(path.relative_to(root))))
        continue
       assert len(matches)==1
       b=matches[0]
       if ch=='input':
        assert any(r[4]=='CELL' and r[7].endswith('~input|o') for r in data)
        # GIN analogue route contains the VIO driver; stop before its increment.
        delay=ps(b[1])-ps(b[2])
       else:
        pad=[r for r in data if r[6]=='PIN_'+pin];assert len(pad)==1
        assert any(r[4]=='CELL' and r[7].endswith('~output|o') for r in data)
        assert any(r[7].endswith('~output|'+('oe' if ch=='oe' else 'i')) for r in data)
        delay=ps(pad[0][1])-ps(b[1])
       assert delay>=0
       rows.append(dict(pin=pin,index=i,channel=ch,project=project,load_pf=load,corner=corner,edge=edge,mode=mode,delay_ps=delay,boundary=native,report=str(path.relative_to(root))))
 models={}
 for i in range(16):
  pin=boundaries[3*i][0];models[pin]={}
  for ch in ('data','oe','input'):
   selected=[r for r in rows if r['index']==i and r['channel']==ch]
   if not selected:continue
   for project in ({r['project'] for r in selected}):
    assert len([r for r in selected if r['project']==project])==16
   assert all(len([r for r in selected if r['project']==f'dq-targeted{load}'])==16 for load in (0,30))
   if ch!='input':assert len(selected)==64
   models[pin][ch]={edge:10*((max(r['delay_ps'] for r in selected if r['edge']==edge)+9)//10) for edge in ('rise','fall')}
 return dict(models=models,rows=rows,rejected_input_boundaries=rejected,report_sha256=hashes)
def compare_native(root, native):
 root=Path(root);native=json.loads(Path(native).read_text())
 models=extract(root)['models']
 for pin,channels in models.items():
  for channel,port in [('data','I'),('oe','OE'),('input','O')]:
   for edge in ('rise','fall'):
    assert native[pin]['models'][port][edge]==[0,channels[channel][edge]],(pin,channel,edge)
 for project in ('dq-native0','dq-native30','dq-targeted0','dq-targeted30'):
  for line in (root/project/'config.tsv').read_text().splitlines():
   pin,key,selection,default,*raw=line.split()
   assert native[pin]['config'][key]==dict(selection=int(selection),raw=[int(x,16) for x in raw]),(pin,key)
 for line in (root/'boundaries.txt').read_text().splitlines():
  pin,gpio,physical,node=line.split()
  port='OE' if ':OEIN' in gpio else 'O' if ':DATAIN' in gpio else 'I'
  pieces=node.split('.')
  assert native[pin]['boundaries'][port]=='.'.join([pieces[0]]+[str(int(x)) for x in pieces[1:]])
 return True

if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('root',nargs='?',default='.');a.add_argument('--output',default='reference.json');v=a.parse_args()
 result=extract(v.root);Path(v.output).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['models'],indent=2))
