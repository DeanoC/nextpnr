#!/usr/bin/env python3
"""Reparse bounded AG5 reference and selector-control fits; no board signoff."""
import hashlib,json,re,sys
from pathlib import Path
CORNERS=('7_slow_1100mv_-40c','7_slow_1100mv_100c','MIN_fast_1100mv_-40c','MIN_fast_1100mv_100c')
def audit(root):
 rows=[]
 for prefix,d5 in (('hdmi',0),('hdmi-native',31)):
  for load in (0,30):
   p=root/f'{prefix}{load}'
   assert 'Full Compilation was successful' in (p/'compile.log').read_text()
   assert '0 errors, 0 warnings' in (p/'paths.log' if prefix=='hdmi-native' or load==30 else p/'paths2.log').read_text()
   qsf=(p/'top.qsf').read_text()
   for text in ('5CSEBA6U23I7','PIN_AG5','"16MA"','-name SLEW_RATE 1',f'-name D5_DELAY {d5} '): assert text in qsf,text
   assert f'-name BOARD_MODEL_FAR_C {load*1e-12} ' in qsf
   config=(p/'config.tsv').read_text()
   expected=f'RB_T9_SEL_OREG_DFF_DELAY\t5\t{1 if d5==0 else 0}\ttype=3\t{d5:02x}'
   assert expected in config,config
   assert 'RB_T9_SEL_EREG_CFF_DELAY\t5\t0\ttype=3\t1f' in config
   assert 'data_inversion\t0\t1' in config
   assert 'boundary\tHMC.025.000:IOINTADDRDOUT.8\tGOUT.037.000.0000' in config
   for field in ('OUTREG_OUTPUT_SEL','OUTREG_MODE_SEL','OEREG_OUTPUT_SEL','INPUT_REG4_SEL'):
    assert re.search(r'^'+field+r'\t\d+\t1\ttype=0$',config,re.M),field
   for corner in CORNERS:
    clocks=(p/corner/'clocks.rpt').read_text()
    assert 'forwarded  ; Generated ; 20.000' in clocks
    assert 'false    ; source ; FPGA_CLK1_50 ; { HDMI_TX_CLK }' in clocks
    for edge in ('rise','fall'):
     for mode in ('early','late'):
      text=(p/corner/f'{edge}-{mode}.rpt').read_text()
      assert 'Found 1 paths.' in text
      assert f'-{edge}_to [get_ports {{HDMI_TX_CLK}}]' in text
      assert ('-min_path' in text)==(mode=='early')
      data=[]
      for line in text.splitlines():
       cols=[x.strip() for x in line.split(';')]
       if len(cols)==9 and re.fullmatch(r'-?\d+\.\d+',cols[1]) and cols[6]:data.append(cols)
      boundary=[x for x in data if x[6]=='BLOCK_INPUT_MUX_PASSTHROUGH_X37_Y0_N0_I0'];assert len(boundary)==1
      pad=[x for x in data if x[6]=='PIN_AG5'];assert len(pad)==1
      ps=lambda v:round(float(v)*1000)
      i=data.index(boundary[0]);j=data.index(pad[0]);assert i<j
      tail=data[i+1:j+1]
      assert any(x[7]=='HMCPHY_RE' for x in tail)
      obuf=[x for x in tail if x[7]=='HDMI_TX_CLK~output|o'];assert len(obuf)==1 and obuf[0][4]=='CELL'
      assert all(x[3] in ('', 'RR' if edge=='rise' else 'FF') for x in tail)
      delay=ps(pad[0][1])-ps(boundary[0][1])
      assert abs(sum(ps(x[2]) for x in tail)-delay)<=3
      if d5==0:assert 0<=delay<=(6960 if edge=='rise' else 6490)
      rows.append(dict(data_d5=d5,load_pf=load,corner=corner,edge=edge,mode=mode,upstream_ps=ps(boundary[0][1]),total_ps=ps(pad[0][1]),tail_ps=delay))
 assert len(rows)==64
 return rows
if __name__=='__main__':
 root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent
 rows=audit(root)
 print(json.dumps(rows,indent=2))
