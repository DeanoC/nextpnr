#!/usr/bin/env python3
"""Audit local GPIO register arcs against the retained TimeQuest path points."""
import argparse
import collections
import json
from pathlib import Path


def summarize(evidence):
    observations = collections.defaultdict(list)
    for variant, data in evidence['variants'].items():
        for report, paths in data['reports'].items():
            for path in paths:
                points = path['points']
                record = {'variant': variant, 'report': report, 'from': path['From Node'], 'to': path['To Node']}
                if '/register-' in report:
                    dst = path['To Node']
                    data_cells = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'data' and
                                  p['type'] == 'CELL' and p['node'] == dst and p['location'].startswith('DDIO')]
                    clocks = [p for p in points if p['section'] == 'required' and p['type'] == 'CELL' and
                              p['node'] == dst and p['location'].startswith('DDIO')]
                    if not data_cells or not clocks:
                        continue
                    family = data_cells[0]['location'].split('_')[0]
                    # Input-register data from the pad is a different boundary
                    # from clock enable and is deliberately excluded here.
                    if family == 'DDIOINCELL' and path['From Node'] not in ['ce', 'ce_n']:
                        continue
                    key = 'input_control' if family == 'DDIOINCELL' else 'output_control'
                    kind = 'hold' if report.endswith('hold.rpt') else 'setup'
                    data_delay = sum(p['incremental_ns'] for p in data_cells)
                    clock_delay = sum(p['incremental_ns'] for p in clocks)
                    intrinsic = sum(p['incremental_ns'] for p in points if p['section'] == 'required' and
                                    p['node'] == dst and p['type'] == ('uTh' if kind == 'hold' else 'uTsu'))
                    value = intrinsic + (clock_delay - data_delay if kind == 'hold' else data_delay - clock_delay)
                    observations[key+'_'+kind].append(dict(record, ns=value))
                if '/fabric-' in report:
                    clocks = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'clock' and
                              p['type'] == 'CELL' and p['location'].startswith('DDIOIN')]
                    cells = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'data' and
                             p['type'] in ['CELL', 'uTco'] and p['location'].startswith('DDIOIN')]
                    if clocks and cells:
                        observations['input_clock_to_fabric'].append(dict(record, ns=sum(p['incremental_ns'] for p in clocks+cells)))
    limits = {'input_control_setup': 120, 'input_control_hold': 660,
              'output_control_setup': 120, 'output_control_hold': 310,
              'input_clock_to_fabric': 870}
    result = {}
    for key, limit_ps in limits.items():
        rows = observations[key]
        if not rows:
            raise ValueError('Missing reference observations: '+key)
        worst = max(rows, key=lambda x: x['ns'])
        if worst['ns']*1000 > limit_ps+1e-6:
            raise ValueError('Backend envelope does not cover '+key+': '+str(worst))
        worst = dict(worst, report_sha256=evidence['variants'][worst['variant']]['hashes'][worst['report']])
        result[key] = {'observed_max_ps': round(worst['ns']*1000, 3), 'envelope_ps': limit_ps,
                       'count': len(rows), 'worst': worst}
    return {'classification': 'local reference envelope; external pad timing remains unsupported',
            'device': evidence['device'], 'bounds': result,
            'input_hashes': {v: {k: h for k, h in d['hashes'].items()
                                if k.endswith('.v') or k in ['top.qsf', 'clocks.sdc']}
                             for v, d in evidence['variants'].items()}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = summarize(json.loads(args.evidence.read_text()))
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS: local fabric-register envelope covers every selected TimeQuest observation')
