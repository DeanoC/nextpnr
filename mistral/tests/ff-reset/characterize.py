#!/usr/bin/env python3
"""Rebuild the FF reset references with Quartus17.0.2; never programs hardware."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from summarize import FILES, digest, source_digest, summarize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quartus-bin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    version = subprocess.check_output([str(args.quartus_bin / 'quartus_sh'), '--version'], text=True)
    assert 'Version 17.0.2 Build 602' in version, version
    args.output.mkdir(parents=True, exist_ok=False)
    projects = {}
    for name in ('rates', 'slots', 'slots-ce'):
        project = args.output / name
        project.mkdir()
        for filename in FILES:
            shutil.copy2(root / 'fixtures' / name / filename, project / filename)
        binding = dict(quartus_version=version, source_sha256={n: source_digest(project / n) for n in FILES},
                       characterizer_sha256=digest(Path(__file__).resolve()), summarizer_sha256=digest(root / 'summarize.py'))
        (project / 'input-binding.json').write_text(json.dumps(binding, indent=2) + '\n')
        for tool, command, logfile in [('quartus_sh', ['--flow', 'compile', 'top'], 'compile.log'),
                                       ('quartus_sta', ['-t', 'audit.tcl'], 'audit.log')]:
            with (project / logfile).open('w') as log:
                subprocess.run([str(args.quartus_bin / tool), *command], cwd=project,
                               stdout=log, stderr=subprocess.STDOUT, check=True)
        result = summarize(project)
        (project / 'reference.json').write_text(json.dumps(result, indent=2) + '\n')
        projects[name] = dict(reference_sha256=digest(project / 'reference.json'), evidence=result)
        print(name, result['observed_max_ns'], flush=True)
    maxima = {kind: max(x['evidence']['observed_max_ns'][kind] for x in projects.values())
              for kind in ('recovery', 'removal')}
    result = dict(scope='Four-corner bounded reference, not silicon, native routing or physical acceptance.',
                  projects=projects, observed_max_ns=maxima, model_ps=dict(recovery=170, removal=270),
                  preserves_historical_hold_floor=True, hardware=False)
    (args.output / 'reference.json').write_text(json.dumps(result, indent=2) + '\n')
    assert maxima['recovery'] <= .170 and maxima['removal'] <= .270, maxima


if __name__ == '__main__':
    main()
