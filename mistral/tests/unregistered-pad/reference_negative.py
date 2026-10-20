#!/usr/bin/env python3
from pathlib import Path
import shutil,tempfile
from reference_check import extract
with tempfile.TemporaryDirectory() as t:
 root=Path(t)
 for name in ('boundaries.txt','defaults.tsv'):shutil.copy2(name,root/name)
 for project in ('dq-native0','dq-native30','dq-targeted0','dq-targeted30'):
  p=root/project;p.mkdir()
  for name in ('config.tsv','top.qsf','compile.log'):shutil.copy2(Path(project)/name,p/name)
  for corner in ('7_slow_1100mv_-40c','7_slow_1100mv_100c','MIN_fast_1100mv_-40c','MIN_fast_1100mv_100c'):
   shutil.copytree(Path(project)/corner,p/corner)
 extract(root)
 path=root/'dq-native0/7_slow_1100mv_100c/0-data-rise-late.rpt';orig=path.read_text()
 for name,change in [('missing-buffer',lambda s:s.replace('~output|o','~output|unknown')),('missing-boundary',lambda s:s.replace('BLOCK_INPUT_MUX_PASSTHROUGH_X39_Y0_N0_I12','BAD_BOUNDARY')),('missing-path',lambda s:s.replace('Found 1 paths.','Found 0 paths.')),('bad-total',lambda s:s.replace(';   1.','; 999.'))]:
  path.write_text(change(orig))
  try:extract(root)
  except (AssertionError,ValueError):print('rejected',name)
  else:raise AssertionError(name)
  path.write_text(orig)
 p=root/'dq-targeted0/7_slow_1100mv_100c/1-input-rise-late.rpt'
 s=p.read_text();p.write_text(s.replace('VIO_BUFFER_X36_Y81_N0_I16','VIO_BUFFER_X36_Y81_N0_I15'))
 try:extract(root)
 except AssertionError:print('rejected targeted-input-boundary')
 else:raise AssertionError('targeted-input-boundary')
 p.write_text(s)
 p=root/'dq-native30/config.tsv';s=p.read_text();p.write_text(s.replace('V12 RB_T9_SEL_EREG_CFF_DELAY 5 0 1f','V12 RB_T9_SEL_EREG_CFF_DELAY 5 0 00'))
 try:extract(root)
 except AssertionError:print('rejected selector-mismatch')
 else:raise AssertionError('selector-mismatch')
