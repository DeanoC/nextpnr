#!/usr/bin/env python3
"""Compare retained Quartus fits with default and forced-31 SDR output delay."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from characterize import paths


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qsf_lines(path):
    return [s.strip() for s in path.read_text().splitlines() if s.strip() and
            not s.startswith('set_global_assignment -name LAST_QUARTUS_VERSION ')]


def output_path(path):
    points = path['points']
    clock = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'clock']
    register_clock = [p for p in clock if p['type'] == 'CELL' and
                      p['location'].startswith('DDIOOUTCELL')]
    data = [p for p in points if p['section'] == 'arrival' and p['stage'] == 'data']
    if len(register_clock) != 1 or not data:
        raise ValueError('Expected a dedicated SDR output register: '+str(path))
    # The network up to the register clock ingress must match between fits.
    prefix = [(p['type'], p['location'], p['node'], p['transition'], p['incremental_ns'])
              for p in clock if p not in register_clock]
    local = sum(p['incremental_ns'] for p in register_clock+data)
    return prefix, local


def fabric_checks(reports):
    result = {}
    for report, rows in reports.items():
        if '/register-' not in report:
            continue
        for row in rows:
            dst = row['To Node']
            if dst not in ['o4', 'o6']:
                continue
            pts = row['points']
            data = [p for p in pts if p['section'] == 'arrival' and p['stage'] == 'data' and
                    p['type'] == 'CELL' and p['node'] == dst and p['location'].startswith('DDIOOUTCELL')]
            clocks = [p for p in pts if p['section'] == 'required' and p['type'] == 'CELL' and
                      p['node'] == dst and p['location'].startswith('DDIOOUTCELL')]
            hold = report.endswith('hold.rpt')
            intrinsic = [p for p in pts if p['section'] == 'required' and p['node'] == dst and
                         p['type'] == ('uTh' if hold else 'uTsu')]
            if not data or not clocks or not intrinsic:
                raise ValueError('Missing local fabric check: '+str(row))
            delta = sum(p['incremental_ns'] for p in data) - sum(p['incremental_ns'] for p in clocks)
            result[(report, row['From Node'], dst)] = sum(p['incremental_ns'] for p in intrinsic) + (-delta if hold else delta)
    if not result:
        raise ValueError('No SDR output fabric setup/hold checks')
    return result


def compare(baseline, delayed):
    for name in ['pads.v', 'clocks.sdc']:
        if digest(baseline/name) != digest(delayed/name):
            raise ValueError('Different source or timing constraints: '+name)
    base = qsf_lines(baseline/'top.qsf')
    modified = qsf_lines(delayed/'top.qsf')
    extra = [s for s in modified if s not in base]
    expected = [f'set_instance_assignment -name {key} 31 -to {port}'
                for port in ['p4', 'p6'] for key in ['D5_DELAY', 'D5_OE_DELAY']]
    if sorted(extra) != sorted(expected) or [s for s in modified if s not in extra] != base:
        raise ValueError('Unexpected assignment differences between fits')
    for port in ['p4', 'p6']:
        warning = f"Can't set option D5_OE Delay Chain to 31 -- option is not used in pin {port} -- changed to 0"
        if warning not in (delayed/'compile.log').read_text():
            raise ValueError('Expected unused OE delay to be reset to zero by Quartus')
    for root, value in [(baseline, '0'), (delayed, '1f')]:
        decoded = (root/'top.bt').read_text()
        for pin in ['AH24', 'AG23']:
            found = re.findall(r'^s DQS16\.\S+:RB_T9_SEL_OREG_DFF_DELAY\.\d+ (\S+) ; '+pin+r'$', decoded, re.M)
            actual = found[0] if len(found) == 1 else '0' if not found else None
            if actual != value:
                raise ValueError('Incorrect fitted data delay selector: '+str((root, pin, actual)))
    def reports(root):
        return {str(p.relative_to(root)): paths(p.read_text()) for p in sorted(root.glob('*_*mv_*c/*.rpt'))}
    a, b = reports(baseline), reports(delayed)
    ac, bc = fabric_checks(a), fabric_checks(b)
    if ac.keys() != bc.keys() or any(abs(ac[k]-bc[k]) > 1e-6 for k in ac):
        raise ValueError('Fabric-facing register setup/hold changed')
    rows = []
    for report, entries in a.items():
        if '/output-' not in report:
            continue
        for row in entries:
            if row['To Node'] not in ['p4', 'p6']:
                continue
            matches = [r for r in b[report] if r['From Node'] == row['From Node'] and r['To Node'] == row['To Node']]
            if len(matches) != 1:
                raise ValueError('Missing matched output path')
            prefix_a, local_a = output_path(row)
            prefix_b, local_b = output_path(matches[0])
            if prefix_a != prefix_b:
                raise ValueError('Reported register clock path changed between fits')
            delta = local_b-local_a
            if delta <= 0:
                raise ValueError('Forced maximum delay did not increase clock-to-pin delay')
            rows.append(dict(report=report, port=row['To Node'], default_ns=round(local_a, 3),
                             forced31_ns=round(local_b, 3), extra_ns=round(delta, 3)))
    if len(rows) != 16 or len({r['report'].split('/')[0] for r in rows}) != 4:
        raise ValueError('Expected two pins, setup/hold and four corners')
    names = ['pads.v', 'top.qsf', 'clocks.sdc', 'compile.log', 'top.bt'] + sorted(
        {r['report'] for r in rows} | {r for r in a if '/register-' in r})
    return {'classification': 'matched Quartus reference fits; not SDRAM hardware signoff',
            'device': '5CSEBA6U23I7', 'fabric_checks_unchanged': len(ac), 'paths': rows,
            'hashes': {label: {n: digest(root/n) for n in names}
                       for label, root in [('default', baseline), ('forced31', delayed)]}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--delayed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.baseline, args.delayed)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS: forced D5=31 increases clock-to-pin delay; fabric setup/hold stays unchanged at all four corners')
