#!/usr/bin/env python3
import shutil,sys,tempfile
from pathlib import Path
from reference_check import audit,CORNERS
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent
cases=('load','data_selector','oe_selector','inversion','corner','edge','boundary','early_query','buffer')
for case in cases:
 with tempfile.TemporaryDirectory() as tmp:
  dest=Path(tmp)
  for name in ('hdmi0','hdmi30','hdmi-native0','hdmi-native30'):
   p=dest/name;p.mkdir()
   for n in ('top.qsf','config.tsv','compile.log','paths.log','paths2.log'):
    if (root/name/n).exists():shutil.copy2(root/name/n,p/n)
   for corner in CORNERS:shutil.copytree(root/name/corner,p/corner)
  p=dest/'hdmi30'
  if case=='load':file=p/'top.qsf';a='3e-11';b='4e-11'
  elif case=='data_selector':file=p/'config.tsv';a='RB_T9_SEL_OREG_DFF_DELAY\t5\t1\ttype=3\t00';b=a[:-2]+'1f'
  elif case=='oe_selector':file=p/'config.tsv';a='RB_T9_SEL_EREG_CFF_DELAY\t5\t0\ttype=3\t1f';b=a[:-2]+'00'
  elif case=='inversion':file=p/'config.tsv';a='data_inversion\t0';b='data_inversion\t1'
  elif case=='corner':(p/CORNERS[0]/'fall-early.rpt').unlink()
  else:
   file=p/CORNERS[0]/('rise-early.rpt' if case=='early_query' else 'rise-late.rpt')
   a,b={'edge':('-rise_to','-fall_to'),'boundary':('BLOCK_INPUT_MUX_PASSTHROUGH_X37_Y0_N0_I0','WRONG_BOUNDARY'),
        'early_query':('-min_path','-max_path'),'buffer':('HDMI_TX_CLK~output|o','missing_output_buffer')}[case]
  if case!='corner':
   text=file.read_text();assert a in text,(case,a);file.write_text(text.replace(a,b))
  try:audit(dest)
  except (AssertionError,FileNotFoundError):pass
  else:raise AssertionError('accepted corruption: '+case)
 print('rejected',case)
print('PASS: 9 reference semantic corruptions rejected')
