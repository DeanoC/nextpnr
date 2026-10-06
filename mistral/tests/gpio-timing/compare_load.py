#!/usr/bin/env python3
"""Audit a matched lumped-load sweep of retained Quartus pad fits."""
import argparse
import hashlib
import json
from pathlib import Path

from pad_summary import arc


def lines(path):
    return [s for s in path.read_text().splitlines() if s.strip() and
            not s.startswith('set_global_assignment -name LAST_QUARTUS_VERSION ')]


def compare(baseline, loaded):
    a = json.loads((baseline/'evidence.json').read_text())
    b = json.loads((loaded/'evidence.json').read_text())
    load = b['output_load_pf']
    if load <= a.get('output_load_pf', 0) or a['device'] != b['device']:
        raise ValueError('Expected the same device and an increased load')
    rows = []
    for variant, data in b['variants'].items():
        for source in [variant+'.v', 'clocks.sdc']:
            if (baseline/variant/source).read_bytes() != (loaded/variant/source).read_bytes():
                raise ValueError('Different reference source or constraints')
        before = lines(baseline/variant/'top.qsf')
        after = lines(loaded/variant/'top.qsf')
        extra = [s for s in after if s not in before]
        prefix = f'set_instance_assignment -name BOARD_MODEL_FAR_C {load*1e-12:.12g} -to '
        if (not extra or any(not s.startswith(prefix) for s in extra) or
                [s for s in after if s not in extra] != before):
            raise ValueError('Unexpected fitted assignment differences')
        ports = {s[len(prefix):] for s in extra}
        for report, paths in data['reports'].items():
            if '/output-' not in report:
                continue
            kind = 'hold' if report.endswith('hold.rpt') else 'setup'
            base_paths = a['variants'][variant]['reports'][report]
            for path in paths:
                extracted = arc(path, kind, True)
                if extracted is None:
                    continue
                matches = []
                for p in base_paths:
                    if p['From Node'] != path['From Node'] or p['To Node'] != path['To Node']:
                        continue
                    candidate = arc(p, kind, True)
                    if candidate is not None and candidate[0] == extracted[0]:
                        matches.append(p)
                if len(matches) != 1 or path['To Node'] not in ports:
                    raise ValueError('Missing unique matched/loaded output path')
                reference = arc(matches[0], kind, True)
                if reference is None or reference[0] != extracted[0]:
                    raise ValueError('Output register channel changed')
                before_arc, after_arc = reference[1], extracted[1]
                if before_arc['clock_edge'] != after_arc['clock_edge']:
                    raise ValueError('Launch edge changed')
                if before_arc['clock_local_ps'] != after_arc['clock_local_ps']:
                    raise ValueError('Local register clock delay changed')
                delta = round(after_arc['value_ps']-before_arc['value_ps'], 3)
                if delta <= 0:
                    raise ValueError('Added load did not increase complete clock-to-pin delay')
                rows.append(dict(variant=variant, report=report, channel=extracted[0],
                                 source=path['From Node'], port=path['To Node'],
                                 baseline_ps=before_arc['value_ps'], loaded_ps=after_arc['value_ps'],
                                 increase_ps=delta))
        # Every baseline registered output path must also appear in the sweep.
        expected = sum(arc(p, 'hold' if r.endswith('hold.rpt') else 'setup', True) is not None
                       for r, ps in a['variants'][variant]['reports'].items() if '/output-' in r for p in ps)
        if sum(r['variant'] == variant for r in rows) != expected:
            raise ValueError('Missing loaded output paths')
    if not rows:
        raise ValueError('No matched registered outputs')
    return dict(classification='matched lumped-load reference fits; not SDRAM board or hardware signoff',
                device=b['device'], baseline_load_pf=a.get('output_load_pf', 0), loaded_pf=load,
                count=len(rows), minimum_increase_ps=min(r['increase_ps'] for r in rows),
                maximum_increase_ps=max(r['increase_ps'] for r in rows), paths=rows,
                evidence_sha256={label: hashlib.sha256((root/'evidence.json').read_bytes()).hexdigest()
                                 for label, root in [('baseline', baseline), ('loaded', loaded)]})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--loaded', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compact', action='store_true', help='Keep extrema witnesses instead of every matched path')
    args = parser.parse_args()
    result = compare(args.baseline, args.loaded)
    if args.compact:
        rows = result.pop('paths')
        result['path_extrema'] = {
            'least_increase': min(rows, key=lambda r: r['increase_ps']),
            'greatest_increase': max(rows, key=lambda r: r['increase_ps']),
            'earliest_loaded': min(rows, key=lambda r: r['loaded_ps']),
            'latest_loaded': max(rows, key=lambda r: r['loaded_ps'])}
        result['channel_counts'] = {channel: sum(r['channel'] == channel for r in rows)
                                    for channel in sorted({r['channel'] for r in rows})}
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(f"PASS: {result['count']} matched data/OE paths have larger clock-to-pin delay with added load")
