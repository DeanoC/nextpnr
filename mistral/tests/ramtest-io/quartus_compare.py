#!/usr/bin/env python3
"""Audit a physical forwarded-clock TimeQuest diagnostic, not hardware signoff.

The phase intervals are signed shifts of the existing capture event on fixed
routes. They are not new PLL settings or proof of the controller's read cycle.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gpio-timing'))
from characterize import paths
from pad_summary import summarize

CORNERS = {'7_slow_1100mv_-40c', '7_slow_1100mv_100c',
           'MIN_fast_1100mv_-40c', 'MIN_fast_1100mv_100c'}


def number(block, name):
    match = re.search(r'^;\s*'+re.escape(name)+r'\s*;\s*([^;]+);', block, re.M)
    if not match:
        raise ValueError('missing '+name)
    value = float(match[1].split()[0])
    if not math.isfinite(value):
        raise ValueError('nonfinite '+name)
    return value


def forwarded_edge(path, section):
    chain = [p for p in path['points'] if p['section'] == section and p['stage'] == 'clock']
    mux = [i for i, p in enumerate(chain) if p['node'].endswith('|muxsel')]
    if len(mux) != 1:
        raise ValueError('missing/ambiguous physical clock mux')
    i = mux[0]
    local = chain[i:]
    if (len(local) < 3 or local[0]['transition'] != 'FF' or
            not local[1]['node'].endswith('|dataout') or local[1]['transition'] != 'FR' or
            local[-1]['node'] != 'SDRAM_CLK' or local[-1]['location'] != 'PIN_AD20' or
            local[-1]['transition'] != 'RR'):
        raise ValueError('expected fabric falling edge to physical rising SDRAM clock')


def audit(root, rate):
    if {p.name for p in root.iterdir() if p.is_dir()} != CORNERS:
        raise ValueError('expected all four operating corners')
    corners, hashes = {}, {}
    for corner in sorted(CORNERS):
        clock = root/corner/'clocks.rpt'
        hashes[str(clock)] = hashlib.sha256(clock.read_bytes()).hexdigest()
        row = next((line for line in clock.read_text().splitlines()
                    if re.match(r';\s*sdram_board\s*;', line)), None)
        if row is None:
            raise ValueError('missing generated board clock')
        cols = [c.strip() for c in row.split(';')[1:-1]]
        period, rise, fall = map(float, (cols[2], cols[4], cols[5]))
        if any(abs(a-b) > .002 for a, b in ((period, 1000/rate),
                                          (rise, 500/rate), (fall, 1000/rate))):
            raise ValueError('unexpected inverted board clock waveform')
        reports = {}
        for name in ('input-setup', 'input-hold', 'output-setup', 'output-hold',
                     'capture-handoff-setup', 'capture-handoff-hold'):
            file = root/corner/(name+'.rpt')
            text = file.read_text()
            hashes[str(file)] = hashlib.sha256(file.read_bytes()).hexdigest()
            parsed = paths(text)
            blocks = re.split(r'^Path #\d+:', text, flags=re.M)[1:]
            if len(parsed) != (52 if name.startswith('output') else 16):
                raise ValueError('incomplete '+name+' coverage')
            if name.startswith('input') and {p['From Node'] for p in parsed} != {
                    f'SDRAM_DQ[{i}]' for i in range(16)}:
                raise ValueError('incomplete DQ coverage')
            if name.startswith('output'):
                expected = Counter({f'SDRAM_DQ[{i}]': 2 for i in range(16)})
                expected.update([f'SDRAM_A[{i}]' for i in range(13)] +
                                ['SDRAM_BA[0]', 'SDRAM_BA[1]', 'SDRAM_CKE',
                                 'SDRAM_nCS', 'SDRAM_nRAS', 'SDRAM_nCAS', 'SDRAM_nWE'])
                if Counter(p['To Node'] for p in parsed) != expected:
                    raise ValueError('incomplete data/OE/command output coverage')
            if name.startswith('capture'):
                target = 'sdram_addon_port:sdram|rdata' if rate == 100 else 'dq_oss_hold'
                if {(p['From Node'], p['To Node']) for p in parsed} != {
                        (f'dq_buf[{i}].dq_sample', f'{target}[{i}]') for i in range(16)}:
                    raise ValueError('unexpected controller capture handoff')
            rows = []
            for path, block in zip(parsed, blocks):
                slack = number(block, 'Slack')
                arrival, required = number(block, 'Data Arrival Time'), number(block, 'Data Required Time')
                expected = required-arrival if name.endswith('setup') else arrival-required
                if abs(slack-expected) > .002:
                    raise ValueError('inconsistent slack arithmetic')
                if name.startswith(('input', 'output')):
                    forwarded_edge(path, 'arrival' if name.startswith('input') else 'required')
                    clock_name = path['Launch Clock'] if name.startswith('input') else path['Latch Clock']
                    if clock_name != 'sdram_board':
                        raise ValueError('unexpected physical clock reference')
                credits = re.findall(r'^;\s*-?\d+\.\d+\s*;\s*(-?\d+\.\d+)\s*;[^\n]*;\s*clock pessimism removed\s*;', block, re.M)
                if len(credits) != 1:
                    raise ValueError('expected one explicit clock pessimism adjustment')
                credit = abs(float(credits[0]))
                rows.append(dict(source=path['From Node'], target=path['To Node'],
                                 slack_ns=slack, clock_pessimism_credit_ns=credit,
                                 slack_without_credit_ns=round(slack-credit, 6)))
            reports[name] = dict(paths=len(rows), worst=min(rows, key=lambda r: r['slack_ns']),
                                 worst_without_credit=min(rows, key=lambda r: r['slack_without_credit_ns']))
        corners[corner] = reports
    lower = max(-c['input-setup']['worst']['slack_ns'] for c in corners.values())
    upper = min(c['input-hold']['worst']['slack_ns'] for c in corners.values())
    lower_without = max(-c['input-setup']['worst_without_credit']['slack_without_credit_ns']
                        for c in corners.values())
    upper_without = min(c['input-hold']['worst_without_credit']['slack_without_credit_ns']
                        for c in corners.values())
    return dict(memory_mhz=rate, corners=corners, report_sha256=hashes,
                fixed_route_capture_event_shift_ns=[lower, upper],
                capture_shift_intersection_width_ns=round(upper-lower, 6),
                capture_shift_without_clock_credit_ns=[lower_without, upper_without],
                capture_shift_width_without_clock_credit_ns=round(upper_without-lower_without, 6),
                common_shift_exists=lower <= upper,
                forwarded_edge_check='fabric FF -> DDIO FR -> physical SDRAM_CLK RR, PIN_AD20')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reports', type=Path, required=True)
    parser.add_argument('--pad-evidence', type=Path, required=True)
    parser.add_argument('--memory-mhz', type=int, choices=(100, 130), required=True)
    parser.add_argument('--inverter-max-ns', type=float, required=True,
                        help='Record the same conditional assumption passed to quartus_board.tcl')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.inverter_max_ns) or args.inverter_max_ns < 0:
        raise ValueError('inverter assumption must be finite and nonnegative')
    result = audit(args.reports, args.memory_mhz)
    summary = summarize(json.loads(args.pad_evidence.read_text()))
    if summary['device'] != '5CSEBA6U23I7' or summary['output_load_pf'] != 30:
        raise ValueError('expected qualified device and 30pF reference load')
    result['local_pad_observations_ps'] = {
        name: [row['minimum']['value_ps'], row['maximum']['value_ps']]
        for name, row in summary['bounds'].items()}
    result['pad_evidence_sha256'] = hashlib.sha256(args.pad_evidence.read_bytes()).hexdigest()
    result['audit_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result['classification'] = 'fixed-route Quartus OSS-RTL diagnostic; no native-model qualification or hardware signoff'
    result['conditions'] = '30pF fitted pad load; flight 0..0.5ns; margin 0.2ns; Quartus PLL uncertainty also retained'
    result['conditional_inverter_max_ns'] = args.inverter_max_ns
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('Capture shift intersection width:', result['capture_shift_intersection_width_ns'], 'ns')


if __name__ == '__main__':
    main()
