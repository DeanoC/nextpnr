#!/usr/bin/env python3
"""Summarize fitted HPS hold references; do not derive silicon minimum delays."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gpio-timing'))
from characterize import paths

CORNERS = {'7_slow_1100mv_-40c', '7_slow_1100mv_100c',
           'MIN_fast_1100mv_-40c', 'MIN_fast_1100mv_100c'}


def summarize(root):
    result = {}
    for corner in sorted(CORNERS):
        cases = {}
        for name in ('input-setup', 'input-hold', 'output-setup', 'output-hold'):
            file = root / corner / (name + '.rpt')
            rows = paths(file.read_text())
            if not rows:
                raise ValueError('No physical paths in ' + str(file))
            slacks = [float(row['Slack']) for row in rows]
            pins, cells = set(), []
            returned = {str(i): [] for i in range(3)}
            for row in rows:
                for point in row['points']:
                    if (point['section'] == 'arrival' and point['stage'] == 'data' and
                            point['type'] == 'CELL' and re.search(
                                r'\|f2sdram\|(?:rd_data|rd_valid|cmd_ready|wrack_data|wrack_valid|wr_ready)_',
                                point['node'])):
                        pins.add(point['node'])
                        cells.append(point['incremental_ns'])
                for i in range(3):
                    if row['To Node'].endswith(f'port{i}|returned'):
                        returned[str(i)].append(float(row['Slack']))
            if name.startswith('output') and not cells:
                raise ValueError('No HPS output atom pins in ' + str(file))
            cases[name] = dict(
                sha256=hashlib.sha256(file.read_bytes()).hexdigest(),
                path_count=len(rows), worst_slack_ns=min(slacks),
                violating_paths=sum(value < 0 for value in slacks),
                output_pins_in_report=len(pins),
                output_cell_delay_min_ns=min(cells) if cells else None,
                output_cell_delay_max_ns=max(cells) if cells else None,
                returned_hold_slack_ns={i: min(values) for i, values in returned.items() if values}
                if name == 'output-hold' else {})
        result[corner] = cases
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reports', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = dict(classification='Fitted Quartus reference, different routes from OSS; '
                                'reports are limited to 2000 paths per case',
                  corners=summarize(args.reports))
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
