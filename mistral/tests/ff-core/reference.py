#!/usr/bin/env python3
"""Read complete fitted FF pin observations; no native or physical acceptance."""
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile

CORNERS = ('7_slow_1100mv_100c', '7_slow_1100mv_-40c',
           'MIN_fast_1100mv_100c', 'MIN_fast_1100mv_-40c')
SOURCE_FILES = ('top.sv', 'top.qsf', 'top.qpf', 'clocks.sdc')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_digest(path):
    data = Path(path).read_bytes()
    if Path(path).name == 'top.qsf':
        data = ('\n'.join(line for line in data.decode().splitlines() if line.strip() and
                         not re.fullmatch(r'set_global_assignment -name LAST_QUARTUS_VERSION "17\.0\.2 Lite Edition"', line)) + '\n').encode()
    return hashlib.sha256(data).hexdigest()


def fixture(root, name):
    return json.loads(gzip.decompress((Path(root) / 'fixtures' / name / 'fixture.json.gz').read_bytes()))


def columns(text):
    result = []
    for line in text.splitlines():
        if not line.startswith(';'):
            continue
        values = [value.strip() for value in line.strip(';').split(';')]
        if len(values) != 7:
            continue
        try:
            numbers = [float(values[0]), float(values[1])]
        except ValueError:
            continue
        assert all(math.isfinite(v) for v in numbers), values
        result.append(values)
    return result


def parse_report(text, spec, role, analysis, edge, corner):
    assert corner in CORNERS and role in spec['report_roles']
    assert analysis in (('recovery', 'removal') if role == 'ACLR' else ('setup', 'hold'))
    assert edge in (('rise',) if role == 'ACLR' else ('rise', 'fall'))
    expected = {site['cell']: site for site in spec['sites']}
    seen, result = set(), []
    for block in re.split(r'\nPath #\d+:', text)[1:]:
        def field(name):
            values = re.findall(r'(?m)^; ' + name + r'\s*;([^;\n]+)', block)
            assert len(values) == 1, (name, values)
            return values[0].strip()
        source, target = field('From Node'), field('To Node')
        cell = source if role == 'Q' else target
        assert cell in expected and cell not in seen, cell
        seen.add(cell)
        site = expected[cell]
        physical = site['location']
        x, y, _ = re.fullmatch(r'FF_X(\d+)_Y(\d+)_N(\d+)', physical).groups()
        clock = site['clock'] + (' (INVERTED)' if site['clock_inverted'] else '')
        assert field('Launch Clock' if role == 'Q' else 'Latch Clock') == clock
        if role == 'ACLR':
            assert source == 'reset_n'
            assert field('Launch Clock') == 'memory_clock|general[2].gpll~PLL_OUTPUT_COUNTER|divclk'
        arrival, required = block.split('; Data Arrival Path')[1].split('; Data Required Path')
        ar, rr = columns(arrival), columns(required)

        def one(values, kind, element):
            found = [r for r in values if r[3] == kind and r[6] == element]
            assert len(found) == 1, (cell, kind, element, found)
            assert found[0][5] == physical
            return found[0]

        item = dict(cell=cell, location=physical, block=site['block'], ff_index=site['ff_index'],
                    corner=corner, role=role, analysis=analysis, edge=edge, clock=clock,
                    clock_inverted=site['clock_inverted'])
        if role == 'Q':
            assert target == f'probes[{site["data_index"]}]'
            cp, qp = one(ar, 'IC', cell + '|clk'), one(ar, 'CELL', cell + '|q')
            anchors = [r for r in ar if r[5].startswith(f'LAB_INPUT_X{x}_Y{y}_')]
            assert len(anchors) == 1 and qp[2] == ('RR' if edge == 'rise' else 'FF')
            clock_cell = one(ar, 'CELL', cell)
            pin = float(qp[0]) - float(cp[0])
            assert abs(pin - float(clock_cell[1])) < .0011 and float(qp[1]) == 0
            item.update(clock_pin_rf=cp[2], clock_pin_to_q_ns=round(pin, 6),
                        clock_anchor_to_q_ns=round(float(qp[0]) - float(anchors[0][0]), 6),
                        clock_pin_ic_increment_ns=float(cp[1]), clock_cell_ns=float(clock_cell[1]))
        else:
            if role == 'din':
                assert source == f'din[{site["data_index"]}]'
                pins = ('d', 'asdata')
            elif role == 'enable':
                assert source == site['enable_input']
                pins = ('ena',)
            elif role == 'sclear':
                assert source == site['clear_input']
                pins = ('sclr',)
            else:
                pins = ('clrn',)
            data_pins = [r for r in ar if r[3] == 'IC' and r[6] in tuple(cell + '|' + p for p in pins)]
            assert len(data_pins) == 1 and data_pins[0][5] == physical
            dp = data_pins[0]
            if role == 'ACLR':
                assert len(dp[2]) == 2 and dp[2][-1] == 'R', dp
            cp = one(rr, 'IC', cell + '|clk')
            dc, cc = one(ar, 'CELL', cell), one(rr, 'CELL', cell)
            setup = analysis in ('setup', 'recovery')
            adjustment = one(rr, 'uTsu' if setup else 'uTh', cell)
            data_delay, clock_delay, adjust = float(dc[1]), float(cc[1]), float(adjustment[1])
            # Preserve the sign reported for the adjustment. Ordinary TimeQuest
            # setup entries subtract uTsu in the required path.
            constraint = data_delay - clock_delay - adjust if setup else clock_delay + adjust - data_delay
            item.update(source=source, physical_pin=dp[6].split('|')[-1], pin_rf=dp[2],
                        clock_pin_rf=cp[2], data_cell_ns=data_delay, clock_cell_ns=clock_delay,
                        intrinsic_adjustment_ns=adjust, effective_pin_constraint_ns=round(constraint, 6))
        slack_text = field('Slack')
        slack = re.fullmatch(r'(-?\d+(?:\.\d+)?)(?: \(VIOLATED\))?', slack_text)
        assert slack, slack_text
        item.update(fitted_slack_ns=float(slack[1]), fitted_violation='(VIOLATED)' in slack_text)
        result.append(item)
    assert seen == set(expected), (role, analysis, edge, len(seen), len(expected))
    return result


def keys(spec):
    for corner in CORNERS:
        for role in spec['report_roles']:
            analyses = ('recovery', 'removal') if role == 'ACLR' else ('setup', 'hold')
            edges = ('rise',) if role == 'ACLR' else ('rise', 'fall')
            for analysis in analyses:
                for edge in edges:
                    yield corner, role, analysis, edge


def summarize_project(project, spec):
    rows, reports = [], {}
    for corner, role, analysis, edge in keys(spec):
        path = Path(project) / 'reports' / corner / f'{role}-{analysis}-{edge}.rpt'
        reports[str(path.relative_to(project))] = digest(path)
        rows.extend(parse_report(path.read_text(encoding='latin1'), spec, role, analysis, edge, corner))
    return dict(observations=rows, reports_sha256=reports)


def check_config(reader, rbf, spec, observations):
    request = []
    for site in spec['sites']:
        x, y, _ = re.fullmatch(r'FF_X(\d+)_Y(\d+)_N(\d+)', site['location']).groups()
        request.append(f'{x} {y} {site["alm"]} {site["ff_index"]}\n')
    before = digest(rbf)
    run = subprocess.run([str(reader), str(rbf)], input=''.join(request), text=True,
                         capture_output=True, check=True)
    assert digest(rbf) == before
    lines, result = run.stdout.splitlines(), []
    assert len(lines) == len(spec['sites'])
    for site, line in zip(spec['sites'], lines):
        x, y, alm, ff, block, pkreg, clock, clock_source, *settings = line.split('\t')
        assert block == site['block'] and int(alm) == site['alm'] and int(ff) == site['ff_index']
        assert site['location'] == f'FF_X{x}_Y{y}_N{6*int(alm)+[1,2,4,5][int(ff)]}'
        mode = dict(value.split('=') for value in settings)
        assert clock_source in ('CLKA', 'CLKB')
        assert int(mode[f'CLK{clock}_INV']) == int(site['clock_inverted'])
        dynamic_enable = site.get('dynamic_enable', 'enable_input' in site)
        assert int(mode[f'EN{clock}_EN']) == int(dynamic_enable)
        if dynamic_enable:
            assert int(mode[f'EN{clock}_NINV']) == int(site['enable_inverted'])
        pins = {r['physical_pin'] for r in observations if r['cell'] == site['cell'] and r['role'] == 'din'}
        assert pins == {'asdata' if pkreg == '1' else 'd'}, (site['cell'], pins, pkreg)
        disabled = mode['SCLR_DIS'] == '1' or mode['TSCLR_DIS' if int(ff)//2 == 0 else 'BSCLR_DIS'] == '1'
        if 'sclear' in spec['report_roles']:
            assert not disabled
            assert int(mode['SCLR_INV']) == int(site['clear_inverted'])
        else:
            assert disabled
        result.append(dict(cell=site['cell'], clock_index=int(clock), clock_source=clock_source,
                           PKREG=int(pkreg), settings=mode))
    assert {r['clock_index'] for r in result} == set(spec['expected_clock_indices'])
    return result


def validate_cached(root, reference, config_reader=None):
    root = Path(root)
    manifest = json.loads((root / 'manifest.json').read_text())
    assert reference['scope'] == manifest['scope'] and set(reference['cases']) == set(manifest['cases'])
    count = 0
    for name, case in manifest['cases'].items():
        source = root / 'fixtures' / name
        assert all(source_digest(source / n) == h for n, h in case['source_sha256'].items())
        assert digest(source / 'fixture.json.gz') == case['fixture_sha256']
        assert digest(source / 'fitted.rbf.gz') == case['rbf_gzip_sha256']
        spec = fixture(root, name)
        expected = {(c, r, a, e, s['cell']) for c, r, a, e in keys(spec) for s in spec['sites']}
        observations = reference['cases'][name]['observations']
        actual = {(r['corner'], r['role'], r['analysis'], r['edge'], r['cell']) for r in observations}
        assert len(observations) == len(actual) and actual == expected, name
        assert all(math.isfinite(r.get('effective_pin_constraint_ns', r.get('clock_anchor_to_q_ns'))) for r in observations)
        sites = {s['cell']: s for s in spec['sites']}
        for row in observations:
            site = sites[row['cell']]
            assert row['location'] == site['location'] and row['block'] == site['block']
            assert row['ff_index'] == site['ff_index'] and row['clock_inverted'] == site['clock_inverted']
            assert row['clock'] == site['clock'] + (' (INVERTED)' if site['clock_inverted'] else '')
            if row['role'] != 'Q':
                setup = row['analysis'] in ('setup', 'recovery')
                value = (row['data_cell_ns'] - row['clock_cell_ns'] - row['intrinsic_adjustment_ns'] if setup else
                         row['clock_cell_ns'] + row['intrinsic_adjustment_ns'] - row['data_cell_ns'])
                assert abs(value - row['effective_pin_constraint_ns']) < .0000006
        count += len(observations)
        if config_reader:
            with tempfile.TemporaryDirectory(prefix='mistral-ff-core-') as tmp:
                rbf = Path(tmp) / 'fitted.rbf'
                rbf.write_bytes(gzip.decompress((source / 'fitted.rbf.gz').read_bytes()))
                assert digest(rbf) == case['rbf_sha256']
                check_config(config_reader, rbf, spec, observations)
    return count
