#!/usr/bin/env python3
"""Require AD20 DDIO register and clock-mux paths to share a routing ingress frame."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
from clock_summary import pulse_checks, summarize as clock_summary


def audit(evidence, project):
    if evidence['device'] != '5CSEBA6U23I7' or evidence['ddr_output_pin'] != 'AD20':
        raise ValueError('Unsupported device or clock pad')
    declared = clock_summary(project)
    hashes = dict(declared['hashes'])
    reports = evidence['variants']['ramtest-pads']['reports']
    matched = 0
    maxima = {}
    for folder in sorted((project / 'clock-requirements').glob('*_*mv_*c')):
        for kind in ['setup', 'hold']:
            key = f'{folder.name}/output-{kind}.rpt'
            hashes[key] = evidence['variants']['ramtest-pads']['hashes'][key]
        report = (folder / 'pulse.rpt').read_text()
        checks = pulse_checks(report)
        blocks = re.split(r'^Path #\d+:', report, flags=re.M)[1:]
        for check, block in zip(checks, blocks):
            if 'altddio_out:clk_pad|' not in check['Node'] or check['Type'] == 'Min Period':
                continue
            key = check['Ingress Type']
            maxima[key] = max(maxima.get(key, 0), check['Ingress Required Width'] * 1000)
            section = None
            prefixes = {'late': [], 'early': []}
            for line in block.splitlines():
                if '; Late Clock Arrival Path' in line: section = 'late'
                elif '; Early Clock Arrival Path' in line: section = 'early'
                cols = [x.strip() for x in line.split(';')[1:-1]]
                if not section or len(cols) != 5 or cols[3] not in ['IC', 'CELL']: continue
                if cols[4] == check['Node']: section = None; continue
                prefixes[section].append((cols[3], cols[4], cols[2], float(cols[1])))
            for mode, prefix in prefixes.items():
                if not prefix or prefix[-1][0] != 'IC' or not prefix[-1][1].endswith(('|clkhi', '|clklo')):
                    raise ValueError('Missing DDIO register clock ingress')
                kind = 'setup' if mode == 'late' else 'hold'
                candidates = []
                for path in reports[f'{folder.name}/output-{kind}.rpt']:
                    if path['To Node'] != 'SDRAM_CLK': continue
                    data = [p for p in path['points'] if p['section'] == 'arrival' and p['stage'] == 'data']
                    muxes = [i for i, p in enumerate(data) if p['node'].endswith('|muxsel')]
                    if len(muxes) != 1: raise ValueError('Missing clock mux ingress')
                    mux = data[muxes[0]]
                    if mux['transition'] != prefix[-1][2]: continue
                    candidate = [(p['type'], p['node'], p['transition'], p['incremental_ns'])
                                 for p in data[:muxes[0]+1]]
                    if prefix[-1][1].rsplit('|', 1)[0] != candidate[-1][1].rsplit('|', 1)[0]:
                        raise ValueError('Clock mux and register belong to different primitives')
                    candidate[-1] = (candidate[-1][0], prefix[-1][1], candidate[-1][2], candidate[-1][3])
                    candidates.append(candidate)
                if not candidates: raise ValueError('Missing matching mux clock edge')
                for candidate in candidates:
                    if len(prefix) != len(candidate) or any(a[:3] != b[:3] or not math.isfinite(a[3]) or
                                                           not math.isfinite(b[3]) or abs(a[3]-b[3]) > 0.0011
                                                           for a, b in zip(prefix, candidate)):
                        raise ValueError('Clock mux/register routing frames differ')
                matched += 1
    if matched != 32 or set(maxima) != {'High Pulse Width', 'Low Pulse Width'}:
        raise ValueError(f'Incomplete four-corner clock mux frame coverage ({matched})')
    return {'device': evidence['device'], 'pin': evidence['ddr_output_pin'],
            'classification': 'four-corner fitted clock-ingress frame audit, not hardware signoff',
            'hashes': hashes, 'audit_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'matched_prefixes': matched,
            'ingress_pulse_maximum_ps': {k: round(v, 3) for k, v in maxima.items()},
            'clock_reference': 'shared DDIO clkhi/clklo and muxsel routing ingress; local CELL arcs excluded'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('project', type=Path)
    parser.add_argument('--check-rejections', action='store_true')
    args = parser.parse_args()
    evidence = json.loads(args.evidence.read_text())
    result = audit(evidence, args.project)
    if args.check_rejections:
        for defect in ['delay', 'node', 'transition', 'missing']:
            bad = copy.deepcopy(evidence)
            rows = bad['variants']['ramtest-pads']['reports']['7_slow_1100mv_100c/output-setup.rpt']
            if defect == 'missing':
                rows[:] = [p for p in rows if p['To Node'] != 'SDRAM_CLK']
            else:
                row = next(p for p in rows if p['To Node'] == 'SDRAM_CLK')
                mux = next(p for p in row['points'] if p['node'].endswith('|muxsel'))
                if defect == 'delay': mux['incremental_ns'] += 0.05
                elif defect == 'node': mux['node'] = 'other|muxsel'
                else: mux['transition'] = 'RF'
            try:
                audit(bad, args.project)
            except ValueError:
                continue
            raise AssertionError('Accepted changed clock frame: '+defect)
        result['rejection_cases'] = 4
    print(json.dumps(result, indent=2))
