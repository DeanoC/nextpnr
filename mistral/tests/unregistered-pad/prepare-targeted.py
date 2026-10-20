from pathlib import Path
import re,shutil
p=Path('dq-targeted0');p.mkdir(exist_ok=True)
for name in ('top.v','clocks.sdc','top.qpf','paths.tcl'):shutil.copy2(Path('dq-native0')/name,p/name)
shutil.copy2('routing-probe/top.qsf',p/'top.qsf')
rcf=Path('routing-probe/top.rcf').read_text();bounds=Path('boundaries.txt').read_text().splitlines()
for i in range(16):
 native=bounds[3*i+2].split()[-1];xyz=list(map(int,native.split('.')[1:]))
 pattern=r'(signal_name = dq\['+str(i)+r'\]~input \{)(.*?)(\n\})'
 def fix(m):
  dest=re.search(r'\tdest = .*?;',m[2]).group(0)
  return m[1]+'\n\tzero_or_more, *;\n\tVIO_BUFFER:X%dY%dS0I%d;\n\tzero_or_more, *;\n'%tuple(xyz)+dest+m[3]
 rcf,n=re.subn(pattern,fix,rcf,flags=re.S);assert n==1
(p/'top.rcf').write_text(rcf)
