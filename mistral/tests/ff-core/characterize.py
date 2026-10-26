#!/usr/bin/env python3
"""Rebuild all FF reference cases with Quartus 17.0.2; never programs hardware."""
import argparse
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import time
from reference import SOURCE_FILES, check_config, digest, fixture, source_digest, summarize_project


def audit_script(roles):
    text = '''package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
# These IO clocks expose local primitive arcs, not a board interface contract.
create_clock -name ff_characterizer_only -period 20.0
set_input_delay -clock ff_characterizer_only -max 0.0 [get_ports {din* enable* sclear*}]
set_input_delay -clock ff_characterizer_only -min 0.0 [get_ports {din* enable* sclear*}]
set_output_delay -clock ff_characterizer_only -max 0.0 [get_ports probes*]
set_output_delay -clock ff_characterizer_only -min 0.0 [get_ports probes*]
foreach corner [get_available_operating_conditions] {
 set_operating_conditions $corner
 update_timing_netlist
 file mkdir reports/$corner
 foreach analysis {setup hold} {
  foreach edge {rise fall} {
'''
    if 'Q' in roles:
        text += '   report_timing -$analysis -from [get_registers sink*] -${edge}_to [get_ports probes*] -npaths 1000 -nworst 1 -detail full_path -show_routing -file reports/$corner/Q-$analysis-$edge.rpt\n'
    for role in ('din', 'enable', 'sclear'):
        if role in roles:
            text += f'   report_timing -$analysis -${{edge}}_from [get_ports {role}*] -to [get_registers sink*] -npaths 1000 -nworst 1 -detail full_path -show_routing -file reports/$corner/{role}-$analysis-$edge.rpt\n'
    text += '  }\n }\n'
    if 'ACLR' in roles:
        text += ''' foreach kind {recovery removal} {
  report_timing -$kind -rise_from [get_registers reset_n] -to [get_registers sink*] -npaths 1000 -nworst 1 -detail full_path -show_routing -file reports/$corner/ACLR-$kind-rise.rpt
 }
'''
    return text + '}\nproject_close\n'


def characterize(root, output, quartus_bin, config_reader):
    root, output, quartus_bin = Path(root), Path(output).absolute(), Path(quartus_bin)
    manifest = json.loads((root / 'manifest.json').read_text())
    version = subprocess.check_output([str(quartus_bin / 'quartus_sh'), '--version'], text=True)
    assert 'Version 17.0.2 Build 602' in version, version
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    result = dict(schema=1, scope=manifest['scope'], device=manifest['device'],
                  quartus_version=version, source_manifest_sha256=digest(root / 'manifest.json'),
                  cases={}, hardware=False, physical_qualified=False)
    for name, case in manifest['cases'].items():
        source, project = root / 'fixtures' / name, output / name
        project.mkdir()
        assert digest(source / 'fixture.json.gz') == case['fixture_sha256']
        spec = fixture(root, name)
        for filename in SOURCE_FILES:
            assert source_digest(source / filename) == case['source_sha256'][filename]
            shutil.copy2(source / filename, project / filename)
        (project / 'fixture.json').write_text(json.dumps(spec, indent=2) + '\n')
        (project / 'audit.tcl').write_text(audit_script(spec['report_roles']))
        binding = dict(source_sha256={n: source_digest(project / n) for n in SOURCE_FILES},
                       fixture_sha256=digest(project / 'fixture.json'), audit_sha256=digest(project / 'audit.tcl'),
                       characterizer_sha256=digest(Path(__file__)), reader_sha256=digest(root / 'reference.py'),
                       config_reader_sha256=digest(config_reader))
        (project / 'input-binding.json').write_text(json.dumps(binding, indent=2) + '\n')
        for tool, command, logfile in [('quartus_sh', ['--flow', 'compile', 'top'], 'compile.log'),
                                        ('quartus_sta', ['-t', 'audit.tcl'], 'audit.log'),
                                        ('quartus_cpf', ['-c', 'output_files/top.sof', 'fitted.rbf'], 'cpf.log')]:
            start = time.monotonic()
            with (project / logfile).open('x') as log:
                run = subprocess.run([str(quartus_bin / tool), *command], cwd=project,
                                     stdout=log, stderr=subprocess.STDOUT)
            (project / (logfile + '.receipt.json')).write_text(json.dumps(dict(
                returncode=run.returncode, elapsed_seconds=time.monotonic()-start), indent=2) + '\n')
            if run.returncode:
                raise RuntimeError(f'{name}: {tool} returned {run.returncode}; capture retained at {project}')
        assert all(source_digest(project / n) == h for n, h in binding['source_sha256'].items())
        evidence = summarize_project(project, spec)
        evidence['configuration'] = check_config(config_reader, project / 'fitted.rbf', spec, evidence['observations'])
        evidence.update(input_binding_sha256=digest(project / 'input-binding.json'), rbf_sha256=digest(project / 'fitted.rbf'))
        result['cases'][name] = evidence
        (output / 'progress.json').write_text(json.dumps(dict(completed_cases=list(result['cases'])), indent=2) + '\n')
        print(name, len(evidence['observations']), flush=True)
    raw = (json.dumps(result, separators=(',', ':'), sort_keys=True) + '\n').encode()
    (output / 'reference.json.gz').write_bytes(gzip.compress(raw, mtime=0))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quartus-bin', type=Path, required=True)
    parser.add_argument('--config-reader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    characterize(Path(__file__).resolve().parent, args.output, args.quartus_bin, args.config_reader.resolve())
