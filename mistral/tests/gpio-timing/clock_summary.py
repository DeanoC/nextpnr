#!/usr/bin/env python3
"""Audit declared GPIO primitive clock requirements against TimeQuest checks."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import re


KINDS = {'Min Period': 'period_ns', 'High Pulse Width': 'high_ns', 'Low Pulse Width': 'low_ns'}


def pulse_checks(report):
    result = []
    for block in re.split(r'^Path #\d+:', report, flags=re.M)[1:]:
        row = {}
        for key in ['Node', 'Clock', 'Type', 'Actual Width', 'Required Width', 'Slack']:
            match = re.search(r'^;\s*'+key+r'\s*;\s*([^;]+);', block, re.M)
            if match is None:
                raise ValueError('Missing pulse check property '+key)
            row[key] = match[1].strip()
        if row['Type'] not in KINDS:
            raise ValueError('Unknown clock requirement')
        for key in ['Actual Width', 'Required Width', 'Slack']:
            row[key] = float(row[key].split()[0])
            if not math.isfinite(row[key]):
                raise ValueError('Nonfinite clock check')
        if row['Required Width'] < 0 or abs(row['Actual Width']-row['Required Width']-row['Slack']) > 0.002:
            raise ValueError('Inconsistent pulse check slack')
        if row['Type'] == 'Min Period':
            # Same-edge period checks have no clock path in this report.
            row.update({'Ingress Type': 'Min Period', 'Ingress Required Width': row['Required Width'],
                        'Local Late Clock': None, 'Local Early Clock': None, 'Clock Inverted': None})
            result.append(row)
            continue
        local = {}
        section = None
        previous = None
        for line in block.splitlines():
            if '; Late Clock Arrival Path' in line:
                section, previous = 'late', None
            elif '; Early Clock Arrival Path' in line:
                section, previous = 'early', None
            cols = [p.strip() for p in line.split(';')[1:-1]]
            if section and len(cols) == 5 and cols[3] in ['IC', 'CELL']:
                if cols[3] == 'CELL' and cols[4] == row['Node']:
                    if section in local or previous is None or previous[3] != 'IC' or not previous[4].endswith(
                            ('|clk', '|clkhi', '|clklo')) or len(cols[2]) != 2:
                        raise ValueError('Ambiguous local clock ingress in pulse check')
                    value = float(cols[1])
                    if not math.isfinite(value):
                        raise ValueError('Nonfinite local clock delay')
                    local[section] = (value, cols[2][0] != cols[2][1])
                previous = cols
        if set(local) != {'late', 'early'} or local['late'][1] != local['early'][1]:
            raise ValueError('Missing or inconsistent local clock paths')
        ingress_kind = row['Type']
        if local['late'][1]:
            ingress_kind = 'Low Pulse Width' if ingress_kind == 'High Pulse Width' else 'High Pulse Width'
        # Normalize the pulse requirement to the physical clock routing
        # ingress. Do not credit global clock pessimism removal to this
        # local model. Period checks retain the declared same-edge period.
        row['Ingress Type'] = ingress_kind
        row['Ingress Required Width'] = row['Required Width'] + local['late'][0]-local['early'][0]
        row['Local Late Clock'] = local['late'][0]
        row['Local Early Clock'] = local['early'][0]
        row['Clock Inverted'] = local['late'][1]
        result.append(row)
    if not result:
        raise ValueError('No reported clock checks')
    return result


def summarize(project):
    folders = sorted((project/'clock-requirements').glob('*_*mv_*c'))
    if len(folders) != 4:
        raise ValueError('Expected four clock-requirement corners')
    bounds = collections.defaultdict(list)
    groups = []
    node_sets = []
    check_count = 0
    for folder in folders:
        with (folder/'requirements.tsv').open() as stream:
            rows = list(csv.DictReader(stream, delimiter='\t'))
        nodes = {}
        locations = collections.defaultdict(list)
        for row in rows:
            if row['node'] in nodes:
                raise ValueError('Duplicate declared clock node')
            for field in KINDS.values():
                row[field] = float(row[field])
                if not math.isfinite(row[field]) or row[field] < 0:
                    raise ValueError('Invalid declared clock requirement')
            nodes[row['node']] = row
            locations[row['location']].append(row)
        node_sets.append(set(nodes))
        checks = pulse_checks((folder/'pulse.rpt').read_text())
        seen = set()
        for check in checks:
            key = (check['Node'], check['Type'])
            if key in seen or check['Node'] not in nodes:
                raise ValueError('Duplicate or unknown reported clock node')
            seen.add(key)
            row = nodes[check['Node']]
            field = KINDS[check['Type']]
            if abs(row[field]-check['Required Width']) > 0.001:
                raise ValueError('Declared requirement differs from TimeQuest check')
            family = row['location'].split('_')[0]
            bounds[family+'_'+field].append(dict(corner=folder.name, node=row['node'],
                                                required_ps=round(row[field]*1000, 3),
                                                actual_ps=round(check['Actual Width']*1000, 3),
                                                slack_ps=round(check['Slack']*1000, 3)))
            ingress_field = KINDS[check['Ingress Type']]
            bounds[family+'_ingress_'+ingress_field].append(dict(
                corner=folder.name, node=row['node'], required_ps=round(check['Ingress Required Width']*1000, 3),
                clock_inverted=check['Clock Inverted'], primitive_type=check['Type'],
                local_late_clock_ps=None if check['Local Late Clock'] is None else round(check['Local Late Clock']*1000, 3),
                local_early_clock_ps=None if check['Local Early Clock'] is None else round(check['Local Early Clock']*1000, 3)))
        for node, row in nodes.items():
            for kind, field in KINDS.items():
                if row[field] > 0 and (node, kind) not in seen:
                    raise ValueError('Missing reported primitive clock check')
        check_count += len(checks)
        for location, rows in locations.items():
            if not location.startswith('DDIOINCELL'):
                continue
            low = [r for r in rows if '|dataout_l[' in r['node']]
            high = [r for r in rows if '|dataout_h[' in r['node']]
            falling = [r for r in rows if r['node'].endswith('~DFFLO')]
            if not low and not high and not falling:
                continue  # SDR primitive, not a DDR handoff.
            if len(low) != 1 or len(high) != 1 or len(falling) != 1:
                raise ValueError('Incomplete DDR input timing-node group')
            if low[0]['synchronous_sources'] or falling[0]['data_destinations']:
                raise ValueError('DDR handoff has an explicit graph edge; audit its timing separately')
            groups.append(dict(corner=folder.name, location=location, falling_capture=falling[0]['node'],
                               retimed_low=low[0]['node'], rising_capture=high[0]['node'],
                               classification='opaque handoff: no falling fanout or low-word synchronous input'))
    if any(s != node_sets[0] for s in node_sets[1:]):
        raise ValueError('Timing-node set changed between corners')
    for key, rows in bounds.items():
        if len({r['corner'] for r in rows}) != 4:
            raise ValueError('Missing clock requirement corner for '+key)
    files = sorted((project/'clock-requirements').rglob('*'))
    files += [p for p in project.iterdir() if p.suffix in ['.v', '.qsf', '.sdc']]
    return dict(classification='declared primitive clock checks; opaque DDR handoff remains without a path arc',
                node_count=len(node_sets[0]), check_count=check_count,
                bounds={key: dict(count=len(rows), maximum=max(rows, key=lambda r: r['required_ps']))
                        for key, rows in sorted(bounds.items())}, opaque_ddr_groups=groups,
                hashes={str(p.relative_to(project)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in files if p.is_file()},
                query_sha256=hashlib.sha256(Path(__file__).with_name('clock_requirements.tcl').read_bytes()).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = summarize(args.project)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(f"PASS: {result['check_count']} clock checks match declared requirements at all four corners")
