#!/usr/bin/env python3
import json,re,hashlib
from pathlib import Path
CORNERS=('7_slow_1100mv_-40c','7_slow_1100mv_100c','MIN_fast_1100mv_-40c','MIN_fast_1100mv_100c')
def audit(root):
 root=Path(root);rows=[];sources={};hashes={}
 for corner in CORNERS:
  p=root/'hps'/corner
  assert 'No clocks to report.' in (p/'clocks.rpt').read_text()
  records={line.split('\t')[0]:line.split('\t') for line in (p/'launch.tsv').read_text().splitlines()}
  assert set(records)=={'i2c~FF_4791','i2c~FF_4792','i2c~FF_4397'}
  for name in ('i2c~FF_4791','i2c~FF_4792'):
   assert records[name][5]=='i2c~FF_4397'
  assert records['i2c~FF_4397'][5]==''
  assert all(v[2:5]==['0.000']*3 for v in records.values())
  sources[corner]=records
  for edge in ('rise','fall'):
   for mode in ('early','late'):
    path=p/f'{edge}-{mode}.rpt';text=path.read_text();assert 'Found 2 paths.' in text
    values=[]
    for line in text.splitlines():
     c=[x.strip() for x in line.split(';')]
     if len(c)==9 and c[4]=='CELL' and c[7] in ('i2c|out_clk','i2c|out_data'):
      values.append(dict(corner=corner,edge=edge,mode=mode,port=c[7],delay_ps=round(float(c[2])*1000)))
    assert len(values)==2;rows.extend(values)
  for path in p.iterdir():hashes[str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
 return dict(status='unsupported-peripheral-launch',clock_relation=None,numerical_cells_are_not_clock_to_output_bounds=True,source_trace=sources,output_cell_observations=rows,sha256=hashes)
if __name__=='__main__':
 result=audit(Path('.'));Path('hps-reference.json').write_text(json.dumps(result,indent=2)+'\n')
 print('PASS: four corners; numerical output CELL observations; internal source has no synchronous predecessor or constrained clock')
