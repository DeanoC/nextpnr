#!/usr/bin/env python3
"""Fit and retain a complete native-cut GPIO reference with Quartus 17.0.2."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import reference as r


def retain(project, destination, reader):
    project, destination = Path(project), Path(destination)
    destination.mkdir()
    (destination/'fixture').mkdir()
    reports = {str(p.relative_to(project)): p.read_bytes() for p in sorted([
        *project.glob('native/*/input-setup.rpt'), *project.glob('native/*/output-setup.rpt'),
        *project.glob('native-rf/*/*.rpt'), *project.glob('native-clock/*/*.rpt')])}
    binding = r.bindings((project/'top.qsf').read_text(), {n: b.decode('latin1') for n,b in reports.items()})
    r.require(len(binding) == 70, 'Expected all 38 data, 16 OE and 16 input register roles')
    configs = r.configuration(reader, project/'native-reference.rbf', sorted({v['pin'] for v in binding.values()}))
    observations, clocks = [], []
    for name, blob in reports.items():
        r.validate_report(name,blob.decode('latin1'))
        if name.startswith('native-rf/'):
            observations.extend(row for path in r.paths(blob.decode('latin1'))
                                if (row := r.extract(path,binding,configs,name)) is not None)
        elif name.startswith('native-clock/'):
            clocks.extend(row for path in r.paths(blob.decode('latin1'))
                          if (row := r.extract_clock(path,binding,configs,name)) is not None)
        r.write_gzip(destination/'reports'/(name+'.gz'),blob)
    data = dict(binding=binding, configuration=configs, observations=observations, clock_observations=clocks)
    r.validate(data)
    for name in r.SOURCES:
        shutil.copy2(project/name,destination/'fixture'/name)
    r.write_gzip(destination/'fixture/fitted.rbf.gz',(project/'native-reference.rbf').read_bytes())
    r.write_gzip(destination/'reference.json.gz',(json.dumps(data,sort_keys=True,separators=(',',':'))+'\n').encode())
    manifest = dict(schema=1,device='5CSEBA6U23I7',corners=list(r.CORNERS),
        classification='Quartus native-cut GPIO reference; no physical or hardware qualification',
        public_native_corner='slow100',q_rounding_guard_ps=1,constraint_rounding_guard_ps=3,
        source_sha256={n:r.digest(destination/'fixture'/n) for n in r.SOURCES},
        rbf_sha256=r.digest(project/'native-reference.rbf'),rbf_gzip_sha256=r.digest(destination/'fixture/fitted.rbf.gz'),
        reference_gzip_sha256=r.digest(destination/'reference.json.gz'),
        reports_sha256={n:r.digest(project/n) for n in reports},observations=len(observations),
        clock_observations=len(clocks),register_roles=len(binding),package_pins=len(configs),
        reference_compiler='Quartus 17.0.2 Build 602',hardware=False)
    (destination/'manifest.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n')
    r.load(destination,reader)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quartus-bin',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--config-reader',type=Path,required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    r.require(not args.output.exists(), 'Output must be a fresh directory')
    version = subprocess.check_output([str(args.quartus_bin/'quartus_sh'),'--version'],text=True)
    r.require('17.0.2 Build 602' in version, 'Reference requires Quartus 17.0.2 Build 602')
    subprocess.run([sys.executable,str(r.ROOT.parent/'gpio-timing/characterize.py'),
        '--quartus-bin',str(args.quartus_bin),'--output',str(args.output),
        '--variants','ramtest-sdr-pads','--zero-input-delays','--all-transitions'],check=True)
    project = args.output/'ramtest-sdr-pads'
    shutil.copy2(r.ROOT/'paths.tcl',project/'native-paths.tcl')
    with (project/'native-paths.log').open('x') as log:
        subprocess.run([str(args.quartus_bin/'quartus_sta'),'-t','native-paths.tcl'],cwd=project,
                       stdout=log,stderr=subprocess.STDOUT,check=True)
    with (project/'native-rbf.log').open('x') as log:
        subprocess.run([str(args.quartus_bin/'quartus_cpf'),'-c','output_files/top.sof','native-reference.rbf'],
                       cwd=project,stdout=log,stderr=subprocess.STDOUT,check=True)
    retain(project,args.output/'reference',args.config_reader)
    print(args.output/'reference')


if __name__ == '__main__':
    main()
