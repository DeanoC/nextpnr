#!/usr/bin/env python3
"""Check declared-clock/pulse-report agreement and opaque DDR graph coverage."""
import argparse
from pathlib import Path
import shutil
import tempfile
import re

from clock_summary import summarize


def check(project):
    reference = summarize(project)
    assert reference['opaque_ddr_groups']
    assert reference['bounds']['DDIOINCELL_period_ns']['maximum']['required_ps'] == 1538
    assert reference['bounds']['DDIOINCELL_ingress_low_ns']['maximum']['required_ps'] == 185
    for kind in ['missing_corner', 'missing_check', 'declaration', 'slack', 'type', 'handoff_edge', 'clock_path']:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            shutil.copytree(project/'clock-requirements', root/'clock-requirements')
            folder = root/'clock-requirements/7_slow_1100mv_100c'
            report = folder/'pulse.rpt'
            declared = folder/'requirements.tsv'
            if kind == 'missing_corner':
                shutil.rmtree(folder)
            elif kind == 'missing_check':
                text = report.read_text()
                report.write_text(text[text.index('Path #2:'):])
            elif kind == 'declaration':
                declared.write_text(declared.read_text().replace('\t1.538\t', '\t1.838\t', 1))
            elif kind in ['slack', 'type', 'clock_path']:
                text = report.read_text()
                start = text.index('Path #1:')
                block = text[start:]
                if kind == 'type':
                    block = block.replace('Low Pulse Width', 'Unknown Check', 1)
                elif kind == 'clock_path':
                    node = re.search(r';\s*Node\s*;\s*([^;]+);', block).group(1).strip()
                    block = re.sub(r'^;[^\n]*;\s*CELL\s*;\s*'+re.escape(node)+r'\s*;\n',
                                   '', block, count=1, flags=re.M)
                else:
                    block = re.sub(r'(;\s*Slack\s*;\s*)[-\d.]+', r'\g<1>1000.000', block, count=1)
                report.write_text(text[:start]+block)
            else:
                rows = declared.read_text().splitlines()
                for i, row in enumerate(rows):
                    if '|dataout_l[' in row:
                        fields = row.split('\t')
                        fields[5] = 'new_explicit_handoff'
                        rows[i] = '\t'.join(fields)
                        break
                declared.write_text('\n'.join(rows)+'\n')
            try:
                summarize(root)
            except ValueError:
                continue
            raise AssertionError('Accepted incomplete or inconsistent '+kind)
    print('PASS: normalized DDR clock requirements and seven incomplete-evidence rejections')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    args = parser.parse_args()
    check(args.project)
