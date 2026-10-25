#!/usr/bin/env python3
"""Rebuild the FF reset references with Quartus17.0.2; never programs hardware."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from summarize import FILES, digest, source_digest, summarize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quartus-bin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    version = subprocess.check_output([str(args.quartus_bin / 'quartus_sh'), '--version'], text=True)
    assert 'Version 17.0.2 Build 602' in version, version
    output = Path(os.path.abspath(args.output))
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)

    staging = Path(tempfile.mkdtemp(prefix=f'.{output.name}.', dir=output.parent))
    try:
        projects = {}
        for name in ('rates', 'slots', 'slots-ce'):
            project = staging / name
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
            evidence = summarize(project)
            if not evidence['all_fitted_checks_pass']:
                raise SystemExit(f'{name}: fitted timing checks failed; refusing to write reference')
            projects[name] = dict(project=project, evidence=evidence)
            print(name, evidence['observed_max_ns'], flush=True)

        maxima = {kind: max(x['evidence']['observed_max_ns'][kind] for x in projects.values())
                  for kind in ('recovery', 'removal')}
        limits = dict(recovery=.170, removal=.270)
        exceeded = {kind: dict(observed_ns=maxima[kind], limit_ns=limit)
                    for kind, limit in limits.items() if maxima[kind] > limit}
        if exceeded:
            raise SystemExit(f'observed reset requirement exceeds model limits: {exceeded}')

        published_projects = {}
        for name, project_result in projects.items():
            project = project_result['project']
            evidence = project_result['evidence']
            reference = project / 'reference.json'
            reference.write_text(json.dumps(evidence, indent=2) + '\n')
            published_projects[name] = dict(reference_sha256=digest(reference), evidence=evidence)
        result = dict(scope='Four-corner bounded reference, not silicon, native routing or physical acceptance.',
                      projects=published_projects, observed_max_ns=maxima,
                      model_ps=dict(recovery=170, removal=270),
                      preserves_historical_hold_floor=True, hardware=False)
        (staging / 'reference.json').write_text(json.dumps(result, indent=2) + '\n')
        if output.exists() or output.is_symlink():
            raise FileExistsError(output)
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


if __name__ == '__main__':
    main()
