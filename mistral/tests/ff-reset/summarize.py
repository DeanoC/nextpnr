#!/usr/bin/env python3
"""Read complete four-corner FF clear references; reject narrowed coverage."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

CORNERS = ('7_slow_1100mv_100c', '7_slow_1100mv_-40c',
           'MIN_fast_1100mv_100c', 'MIN_fast_1100mv_-40c')
FILES = ('top.sv', 'top.qsf', 'top.qpf', 'clocks.sdc', 'audit.tcl', 'fixture.json')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(path):
    data = path.read_bytes()
    if path.name == 'top.qsf':
        # Only Quartus's version bookkeeping and blank lines may change.
        lines = data.decode().splitlines()
        data = ('\n'.join(line for line in lines if line.strip() and
                         not re.fullmatch(r'set_global_assignment -name LAST_QUARTUS_VERSION "17\.0\.2 Lite Edition"', line))
                + '\n').encode()
    return hashlib.sha256(data).hexdigest()


def parse_report(text, kind, corner, expected):
    assert kind in ('recovery', 'removal')
    seen, out = set(), []
    for block in re.split(r'\nPath #\d+:', text)[1:]:
        def field(name):
            values = re.findall(r'(?m)^; ' + name + r'\s*;([^;\n]+)', block)
            assert len(values) == 1, (name, len(values))
            return values[0].strip()
        name = field('To Node')
        assert name in expected and name not in seen, name
        seen.add(name)
        target = expected[name]
        arrival = block.split('; Data Arrival Path')[1].split('; Data Required Path')[0]
        required = block.split('; Data Required Path')[1]

        def rows(section):
            values = []
            for line in section.splitlines():
                parts = [value.strip() for value in line.split(';')[1:-1]]
                if len(parts) != 7:
                    continue
                try:
                    float(parts[0]); float(parts[1])
                except ValueError:
                    continue
                values.append(parts)
            return values

        ar, rr = rows(arrival), rows(required)
        def one(values, typ, element):
            matches = [x for x in values if x[3] == typ and x[6] == element]
            assert len(matches) == 1, (kind, name, typ, element, len(matches))
            return matches[0]
        reset = one(ar, 'CELL', name)
        clock = one(rr, 'CELL', name)
        cp, rp = one(rr, 'IC', name + '|clk'), one(ar, 'IC', name + '|clrn')
        intrinsic = one(rr, 'uTsu' if kind == 'recovery' else 'uTh', name)
        locations = {x[5] for x in (reset, clock, cp, rp, intrinsic)}
        assert len(locations) == 1
        location = next(iter(locations))
        assert target['location'] is None or location == target['location'], (name, location)
        assert field('Launch Clock') == field('Latch Clock') == target['clock']
        assert '(INVERTED)' not in target['clock']
        local = (float(reset[1]) - float(clock[1]) if kind == 'recovery'
                 else float(clock[1]) - float(reset[1])) + float(intrinsic[1])
        slack = float(field('Slack'))
        assert math.isfinite(local) and math.isfinite(slack)
        out.append(dict(target, corner=corner, kind=kind, location=location,
                        local_requirement_ns=round(local, 6), reset_cell_ns=float(reset[1]),
                        clock_cell_ns=float(clock[1]), intrinsic_ns=float(intrinsic[1]), fitted_slack_ns=slack))
    assert seen == set(expected), (kind, len(seen), len(expected))
    return out


def summarize(project):
    binding = json.loads((project / 'input-binding.json').read_text())
    for name, value in binding['source_sha256'].items():
        assert source_digest(project / name) == value, name
    fixture = json.loads((project / 'fixture.json').read_text())
    expected = {x['cell']: x for x in fixture['sites']}
    assert len(expected) == len(fixture['sites'])
    observations, reports = [], {}
    for corner in CORNERS:
        for kind in ('recovery', 'removal'):
            path = project / 'timing-audit' / corner / (kind + '.rpt')
            reports[str(path.relative_to(project))] = digest(path)
            observations.extend(parse_report(path.read_text(encoding='latin1'), kind, corner, expected))
    maxima = {kind: max(x['local_requirement_ns'] for x in observations if x['kind'] == kind)
              for kind in ('recovery', 'removal')}
    return dict(scope='Local fitted FF reset reference; excludes routing, reset pulse width, MTBF and board qualification.',
                input_binding_sha256=digest(project / 'input-binding.json'),
                source_sha256=binding['source_sha256'], reports_sha256=reports, observations=observations,
                observed_max_ns=maxima,
                rounded_outward_10ps={kind: math.ceil(value * 100 - 1e-9) * 10 for kind, value in maxima.items()},
                all_fitted_checks_pass=all(x['fitted_slack_ns'] >= 0 for x in observations), hardware=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = summarize(args.project)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(result['observed_max_ns'])
