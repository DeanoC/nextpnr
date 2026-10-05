#!/usr/bin/env python3
"""Check reference-frame normalization and rejection of incomplete pad evidence."""
import argparse
import copy
import json

from pad_summary import arc, summarize


def check(evidence):
    result = summarize(evidence)
    if evidence.get('all_transitions', False):
        modified = copy.deepcopy(evidence)
        data = next(iter(modified['variants'].values()))
        report = next(r for r in data['reports'] if '/output-rise-setup.rpt' in r)
        del data['reports'][report]
        try:
            summarize(modified)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted missing explicit rise transition coverage')
    if set(evidence['variants']) == {'ddr-output-data'}:
        assert result['bounds']['write_ddr_low_late']['maximum']['clock_edge'] == 'F'
        assert result['bounds']['write_ddr_high_late']['maximum']['clock_edge'] == 'R'
        rows = evidence['variants']['ddr-output-data']['reports']['7_slow_1100mv_100c/output-setup.rpt']
        path = rows[0]
        for missing in ['muxsel', 'dataout', 'IOOBUF', 'phase', 'total']:
            modified = copy.deepcopy(path)
            if missing == 'phase':
                modified['Launch Clock'] = 'memory' if 'INVERTED' in modified['Launch Clock'] else 'memory (INVERTED)'
            elif missing == 'total':
                modified['points'][-1]['total_ns'] += 0.1
            else:
                modified['points'] = [p for p in modified['points'] if not (
                    p['node'].endswith('|'+missing) if missing != 'IOOBUF' else
                    p['type'] == 'CELL' and p['location'].startswith('IOOBUF'))]
            try:
                parsed = arc(modified, 'setup', True)
            except ValueError:
                continue
            raise AssertionError('Accepted incomplete DDR output '+missing+': '+str(parsed))
        print('PASS: both DDR output phases and five incomplete-mux-path rejections')
        return
    assert result['bounds']['read_ddr_low_setup']['maximum']['clock_edge'] == 'F'
    assert result['bounds']['read_ddr_high_setup']['maximum']['clock_edge'] == 'R'
    if 'read_sdr_hold' in result['bounds']:
        assert result['bounds']['read_sdr_hold']['minimum']['value_ps'] < 0
    selected = {}
    variant = 'ramtest-pads' if 'ramtest-pads' in evidence['variants'] else 'ddr'
    for report, rows in evidence['variants'][variant]['reports'].items():
        if report != '7_slow_1100mv_100c/input-setup.rpt' and report != '7_slow_1100mv_100c/output-setup.rpt':
            continue
        output = '/output-' in report
        for row in rows:
            parsed = arc(row, 'setup', output)
            if parsed is not None and not (output and parsed[0].startswith('write_ddr_')):
                selected.setdefault('write' if output else 'read', row)

    def rejected(path, output):
        try:
            arc(path, 'setup', output)
        except ValueError:
            return
        raise AssertionError('Accepted incomplete pad path')

    for name, prefix in [('read', 'IOIBUF'), ('write', 'IOOBUF')]:
        row = copy.deepcopy(selected[name])
        row['points'] = [p for p in row['points'] if not (
            p['section'] == 'arrival' and p['type'] == 'CELL' and p['location'].startswith(prefix))]
        rejected(row, name == 'write')
    for name in ['read', 'write']:
        row = copy.deepcopy(selected[name])
        section = 'required' if name == 'read' else 'arrival'
        row['points'] = [p for p in row['points'] if not (
            p['section'] == section and p['stage'] == 'clock' and p['type'] == 'CELL' and
            p['location'].startswith('DDIO'))]
        rejected(row, name == 'write')
    row = copy.deepcopy(selected['write'])
    next(p for p in row['points'] if p['section'] == 'arrival' and p['stage'] == 'data' and
         p['type'] == 'CELL')['incremental_ns'] += 0.1
    rejected(row, True)
    row = copy.deepcopy(selected['read'])
    row['Latch Clock'] = 'memory' if 'INVERTED' in row['Latch Clock'] else 'memory (INVERTED)'
    rejected(row, False)
    for missing in ['corner', 'early']:
        modified = copy.deepcopy(evidence)
        for data in modified['variants'].values():
            data['reports'] = {k: v for k, v in data['reports'].items() if not (
                k.startswith('MIN_fast_1100mv_100c/') if missing == 'corner' else
                '/output-' in k and k.endswith('hold.rpt'))}
        try:
            summarize(modified)
        except ValueError:
            continue
        raise AssertionError('Accepted missing '+missing+' coverage')
    print('PASS: pad reference normalization, read edges, negative hold and eight incomplete-evidence rejections')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence')
    args = parser.parse_args()
    with open(args.evidence) as stream:
        check(json.load(stream))
