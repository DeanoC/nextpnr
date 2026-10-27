#!/usr/bin/env python3
"""Extract GPIO local arcs at the native GOUT/DCMUX/GIN routing boundaries.

TimeQuest's DDIO CELL rows omit IO_RE and HMCPHY_RE interconnect. Keep those
rows, match every boundary to the fitted physical port, and require both
transitions in setup and hold at every retained corner.
"""
import argparse
import collections
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
CORNERS = ('7_slow_1100mv_100c', '7_slow_1100mv_-40c',
           'MIN_fast_1100mv_100c', 'MIN_fast_1100mv_-40c')
ROLES = ('I', 'OE', 'Q')
SOURCES = ('ramtest-sdr-pads.v', 'top.qsf', 'top.qpf', 'clocks.sdc')
SETTINGS = ('RB_T1_SEL_IREG_CFF_DELAY', 'SET_T3_FOR_CDATA0IN', 'SET_T3_FOR_CDATA1IN',
            'RB_T9_SEL_OREG_DFF_DELAY', 'RB_T9_SEL_EREG_CFF_DELAY',
            'INPUT_REG0_SEL', 'INPUT_REG1_SEL', 'INPUT_REG2_SEL', 'INPUT_REG3_SEL', 'INPUT_REG4_SEL',
            'OUTREG_OUTPUT_SEL', 'OUTREG_MODE_SEL', 'OEREG_OUTPUT_SEL', 'OEREG_HR_CLK_EN',
            'OEREG_CLK_INV', 'RBOE_LVL_FR_CLK_EN', 'RB_FIFO_WCLK_EN', 'RB_FIFO_WCLK_INV',
            'INPUT_PATH_CE_IN', 'CE_OUTREG_TIEOFF_EN', 'CE_OEREG_TIEOFF_EN',
            'USE_CLR_INREG_EN', 'USE_CLR_OUTREG_EN', 'OEREG_ACLR_EN')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_gzip(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(gzip.compress(data, mtime=0))


def ps(text):
    require(re.fullmatch(r'-?\d+\.\d{3}', text), 'Not a finite 1ps report quantity: ' + text)
    negative = text.startswith('-')
    whole, fraction = text.lstrip('-').split('.')
    return (-1 if negative else 1) * (int(whole)*1000 + int(fraction))


def paths(text):
    for block in re.split(r'(?m)^Path #\d+:', text)[1:]:
        def field(name):
            values = re.findall(r'(?m)^; ' + re.escape(name) + r'\s*;([^;\n]+)', block)
            require(len(values) == 1, 'Missing/ambiguous path field: ' + name)
            return values[0].strip()
        row = dict(source=field('From Node'), sink=field('To Node'), points=[])
        section = stage = None
        for line in block.splitlines():
            if '; Data Arrival Path' in line:
                section, stage = 'arrival', None
            elif '; Data Required Path' in line:
                section, stage = 'required', None
            if re.search(r';\s*clock path\s*;', line):
                stage = 'clock'
            elif re.search(r';\s*data path\s*;', line):
                stage = 'data'
            cols = [v.strip() for v in line.split(';')[1:-1]]
            if section and len(cols) == 7 and re.fullmatch(r'-?\d+\.\d+', cols[0]):
                row['points'].append(dict(section=section, stage=stage, total_ps=ps(cols[0]),
                    increment_ps=ps(cols[1]), rf=cols[2], type=cols[3], location=cols[5], element=cols[6]))
        yield row


def one(rows, label):
    require(len(rows) == 1, f'Expected one {label}, found {len(rows)}')
    return rows[0]


def validate_report(name, text):
    corner = name.split('/')[1]
    require(corner in CORNERS, 'Unexpected reported corner')
    expected = ('Slow' if corner.startswith('7_') else 'Fast') + ' 1100mV ' + (
        '-40C' if corner.endswith('_-40c') else '100C') + ' Model'
    model = re.findall(r'(?m)^Delay Model:\n\s*([^\n]+)', text)
    require(len(model) == 1 and model[0].strip() == expected, 'Reported operating condition differs from corner')
    command = re.findall(r'Tcl Command:\n\s*(.+?)\n\nOptions:', text, re.S)
    require(len(command) == 1, 'Missing unique report query')
    command = command[0]
    if name.startswith('native-clock/'):
        require(command.startswith('report_path '), 'Unexpected clock-prefix report command')
        require(('-min_path' in command) == name.endswith('-min.rpt'), 'Wrong early/late clock query')
        edge = 'rise' if '/rise-' in name else 'fall'
        require('-'+edge+'_to ' in command, 'Wrong clock transition query')
    else:
        require(command.startswith('report_timing '), 'Unexpected timing report command')
        kind = 'hold' if name.endswith('-hold.rpt') else 'setup'
        require(re.search(r'\s-'+kind+r'\s', command), 'Wrong setup/hold report query')
        if name.startswith('native-rf/'):
            edge = 'rise' if '-rise-' in name else 'fall'
            direction = 'from' if '/fabric-' in name else 'to'
            require('-'+edge+'_'+direction+' ' in command, 'Wrong data transition query')
    require('-show_routing' in command, 'Missing full routing-element report')


def configuration(reader, rbf, pins):
    result = subprocess.run([str(Path(reader).resolve()), str(Path(rbf).resolve())],
                            input='\n'.join(pins)+'\n', text=True, capture_output=True)
    require(result.returncode == 0, 'Configuration reader failed: ' + result.stderr)
    rows = {}
    for line in result.stdout.splitlines():
        cols = line.split('\t')
        require(len(cols) > 5 and cols[0] not in rows, 'Incomplete/duplicate GPIO configuration')
        values = dict(v.split('=', 1) for v in cols[5:])
        require(all(k in values for k in SETTINGS), 'Missing physical GPIO selector')
        rows[cols[0]] = dict(x=int(cols[1]), y=int(cols[2]), bi=int(cols[3]), lane=cols[4], values=values)
    require(set(rows) == set(pins), 'Missing or unexpected package-pin configuration')
    return rows


def anchor(rows, identity, kind):
    parsed = re.fullmatch(r'(GOUT|DCMUX|GIN)\.(\d+)\.(\d+)\.(\d+)', identity)
    require(parsed is not None, 'Missing physical routing identity: ' + identity)
    typ, x, y, index = parsed.groups()
    expected, prefix = {'clock': ('DCMUX', 'GLOBAL_LOCAL_SEL'),
                        'data': ('GOUT', 'BLOCK_INPUT_MUX(?:_PASSTHROUGH)?'),
                        'q': ('GIN', '(?:VIO|HIO)_BUFFER')}[kind]
    require(typ == expected, 'Wrong physical routing boundary type')
    location = prefix + f'_X{int(x)}_Y{int(y)}_N0_I{int(index)}'
    result = one([r for r in rows if re.fullmatch(location, r['location'])], kind + ' boundary ' + identity)
    require(result['type'] == 'RE', 'Routing boundary is not a physical routing element')
    if kind == 'clock':
        require(result['increment_ps'] == 0, 'Clock mux marker has an unqualified local delay')
    return result


def bindings(qsf, reports):
    pinmap = {port: pin for pin, port in re.findall(
        r'^set_location_assignment PIN_(\w+) -to (\S+)', qsf, re.M)}
    result = {}
    for name, text in reports.items():
        if not re.fullmatch(r'native/[^/]+/(input|output)-setup\.rpt', name):
            continue
        for path in paths(text):
            if '/input-' in name and path['source'] in pinmap:
                selected = [(p, path['source'], 'Q') for p in path['points']
                            if p['type'] == 'CELL' and p['location'].startswith('DDIOINCELL_')
                            and p['element'] == path['sink']]
            elif '/output-' in name and path['sink'] in pinmap:
                selected = [(p, path['sink'], 'I' if p['location'].startswith('DDIOOUTCELL_') else 'OE')
                            for p in path['points'] if p['section'] == 'arrival' and p['stage'] == 'clock'
                            and p['type'] == 'CELL' and p['element'] == path['source']
                            and p['location'].startswith(('DDIOOUTCELL_', 'DDIOOECELL_'))]
            else:
                continue
            for point, port, role in selected:
                entry = dict(pin=pinmap[port], role=role)
                loc = point['location']
                require(loc not in result or result[loc] == entry, 'Conflicting physical register binding')
                result[loc] = entry
    require(result, 'No physical GPIO register bindings')
    return result


def extract(path, binding, configs, report):
    """Return a native local observation, or None for an unrelated register."""
    match = re.fullmatch(r'native-rf/([^/]+)/(register|fabric)-(rise|fall)-(setup|hold)\.rpt', report)
    require(match is not None and match[1] in CORNERS, 'Unexpected report identity: ' + report)
    corner, family, edge, analysis = match.groups()
    arrival = [r for r in path['points'] if r['section'] == 'arrival']
    required = [r for r in path['points'] if r['section'] == 'required']
    if family == 'register':
        selected = [r for r in arrival if r['type'] == 'CELL' and r['element'] == path['sink']
                    and r['location'] in binding and binding[r['location']]['role'] in ('I', 'OE')]
        if not selected:
            return None
        data = one(selected, 'GPIO data CELL')
        loc = data['location']
        site = binding[loc]
        cfg = configs[site['pin']]['values']
        dp = one([r for r in arrival if r['type'] == 'IC' and r['location'] == loc], 'GPIO data pin')
        clock = one([r for r in required if r['type'] == 'CELL' and r['location'] == loc
                     and r['element'] == path['sink']], 'GPIO clock CELL')
        cp = one([r for r in required if r['type'] == 'IC' and r['location'] == loc], 'GPIO clock pin')
        da = anchor(arrival[:arrival.index(dp)], cfg['DATAOUT0' if site['role'] == 'I' else 'OEIN0'], 'data')
        ca = anchor(required[:required.index(cp)], cfg['CLKOUT0'], 'clock')
        intrinsic = one([r for r in required if r['type'] == ('uTsu' if analysis == 'setup' else 'uTh')
                         and r['element'] == path['sink']], 'intrinsic constraint')
        dl, cl = data['total_ps']-da['total_ps'], clock['total_ps']-ca['total_ps']
        value = dl-cl-intrinsic['increment_ps'] if analysis == 'setup' else cl+intrinsic['increment_ps']-dl
        require(len(data['rf']) == 2 and data['rf'][0] == dp['rf'][-1], 'Inconsistent GPIO data polarity')
        query_rf = data['rf'][-1]
        row = dict(role=site['role'], value_ps=value, data_local_ps=dl, clock_local_ps=cl,
                   intrinsic_ps=intrinsic['increment_ps'], data_anchor=da, clock_anchor=ca,
                   clock_prefix_ps=cp['total_ps']-ca['total_ps'], rf=dp['rf'], data_cell_rf=data['rf'])
    else:
        selected = [r for r in arrival if r['type'] == 'CELL' and r['stage'] == 'clock'
                    and r['element'] == path['source'] and r['location'] in binding
                    and binding[r['location']]['role'] == 'Q']
        if not selected:
            return None
        clock = one(selected, 'GPIO source clock CELL')
        loc = clock['location']
        site = binding[loc]
        cfg = configs[site['pin']]['values']
        cp = one([r for r in arrival if r['type'] == 'IC' and r['location'] == loc
                  and r['stage'] == 'clock'], 'GPIO source clock pin')
        qp = one([r for r in arrival if r['type'] == 'CELL' and r['location'] == loc
                  and r['element'] == path['source']+'|q'], 'GPIO Q CELL')
        ca = anchor(arrival[:arrival.index(cp)], cfg['CLKIN0'], 'clock')
        buf = anchor(arrival[arrival.index(qp)+1:], cfg['DATAIN3'], 'q')
        start = arrival[arrival.index(buf)-1]
        require(start['stage'] == 'data' and start['type'] == 'RE', 'Missing GPIO return interconnect')
        row = dict(role='Q', value_ps=start['total_ps']-ca['total_ps'], clock_anchor=ca,
                   clock_prefix_ps=cp['total_ps']-ca['total_ps'], fabric_anchor_input=start,
                   fabric_buffer=buf, rf=qp['rf'])
        query_rf = qp['rf'][-1]
    # An OE register's CELL inverts its output. The explicit query selects
    # that ending state, while setup/hold is indexed by its native input state.
    require(query_rf == ('R' if edge == 'rise' else 'F'), 'Data transition disagrees with explicit query')
    require(row['rf'] in ('RR', 'FF'), 'Unsupported native GPIO data polarity')
    require(row['clock_prefix_ps'] >= 0, 'Negative GPIO clock prefix')
    row.update(pin=site['pin'], register_location=loc, corner=corner, query_edge=edge,
               edge='rise' if row['rf'][-1] == 'R' else 'fall', analysis=analysis,
               report=report, source=path['source'], sink=path['sink'])
    return row


def extract_clock(path, binding, configs, report):
    match = re.fullmatch(r'native-clock/([^/]+)/(rise|fall)-(max|min)\.rpt', report)
    require(match is not None and match[1] in CORNERS, 'Unexpected clock report identity')
    corner, edge, analysis = match.groups()
    selected = [r for r in path['points'] if r['type'] == 'IC' and r['element'] == path['sink']
                and r['location'] in binding and r['element'].endswith('|clk')]
    if not selected:
        return None
    cp = one(selected, 'GPIO clock ingress')
    site = binding[cp['location']]
    cfg = configs[site['pin']]['values']
    ca = anchor(path['points'][:path['points'].index(cp)],
                cfg['CLKIN0' if site['role'] == 'Q' else 'CLKOUT0'], 'clock')
    require(cp['rf'] == ('RR' if edge == 'rise' else 'FF'), 'Unsupported GPIO clock polarity')
    value = cp['total_ps']-ca['total_ps']
    require(value >= 0, 'Negative local GPIO clock delay')
    return dict(pin=site['pin'], role=site['role'], register_location=cp['location'], corner=corner,
                edge=edge, analysis=analysis, value_ps=value, clock_anchor=ca, clock_pin=cp,
                report=report, source=path['source'], sink=path['sink'])


def validate(data):
    binding, rows = data['binding'], data['observations']
    require(set(data['configuration']) == {s['pin'] for s in binding.values()}, 'Incomplete physical pin configuration')
    expected = {(s['pin'], s['role'], c, a, e) for s in binding.values()
                for c in CORNERS for a in ('setup', 'hold') for e in ('rise', 'fall')}
    grouped = collections.defaultdict(list)
    for row in rows:
        grouped[(row['pin'], row['role'], row['corner'], row['analysis'], row['edge'])].append(row)
        require(row['register_location'] in binding and binding[row['register_location']] ==
                {'pin': row['pin'], 'role': row['role']}, 'Mismatched GPIO register binding')
        if row['role'] != 'Q':
            dl, cl, intrinsic = (row[k] for k in ('data_local_ps', 'clock_local_ps', 'intrinsic_ps'))
            require(row['value_ps'] == (dl-cl-intrinsic if row['analysis'] == 'setup' else cl+intrinsic-dl),
                    'Changed native GPIO constraint equation')
    require(set(grouped) == expected, 'Missing or unexpected GPIO corner/analysis/edge coverage')
    # Two q[] ports observe each input register. They must give the same local
    # prefix after excluding their distinct fabric routes.
    for key, samples in grouped.items():
        require(len({r['value_ps'] for r in samples}) == 1, 'Conflicting local observations: ' + str(key))
        require(len(samples) == (2 if key[1] == 'Q' else 1), 'Missing/duplicate observation: ' + str(key))
    clocks = {(r['pin'], r['role'], r['corner'], r['analysis'], r['edge']): r
              for r in data['clock_observations']}
    expected_clocks = {(s['pin'], s['role'], c, a, e) for s in binding.values()
                       for c in CORNERS for a in ('max', 'min') for e in ('rise', 'fall')}
    require(len(clocks) == len(data['clock_observations']) and set(clocks) == expected_clocks,
            'Missing/duplicate local clock corner/edge/early/late coverage')
    for row in rows:
        # Setup captures early clocks and launches late clocks. Both use
        # exactly the same native prefix as the independent report_path query.
        late = (row['analysis'] == 'setup') == (row['role'] == 'Q')
        key = (row['pin'], row['role'], row['corner'], 'max' if late else 'min', 'rise')
        require(abs(row['clock_prefix_ps']-clocks[key]['value_ps']) <= 2,
                'Timing and independent clock-prefix reports disagree')
    return grouped


def clock_envelopes(data):
    validate(data)
    result = {}
    for pin in sorted({s['pin'] for s in data['binding'].values()}):
        corners = []
        for corner in CORNERS:
            roles = []
            for role in ROLES:
                rows = [r for r in data['clock_observations']
                        if (r['pin'], r['role'], r['corner']) == (pin, role, corner)]
                if not rows:
                    roles.append(None)
                    continue
                values = []
                for edge in ('rise', 'fall'):
                    early = one([r for r in rows if r['analysis'] == 'min' and r['edge'] == edge], 'early clock')['value_ps']-1
                    late = one([r for r in rows if r['analysis'] == 'max' and r['edge'] == edge], 'late clock')['value_ps']+1
                    require(0 <= early <= late, 'Reversed local clock interval')
                    values.extend((early, late))
                roles.append(values)
            corners.append(roles)
        result[pin] = corners
    return result


def envelopes(data):
    validate(data)
    result = {}
    for pin in sorted({s['pin'] for s in data['binding'].values()}):
        corners = []
        for corner in CORNERS:
            roles = []
            for role in ROLES:
                rows = [r for r in data['observations'] if (r['pin'], r['role'], r['corner']) == (pin, role, corner)]
                if not rows:
                    roles.append(None)
                elif role == 'Q':
                    values = []
                    for edge in ('rise', 'fall'):
                        early = min(r['value_ps'] for r in rows if r['analysis'] == 'hold' and r['edge'] == edge)-1
                        late = max(r['value_ps'] for r in rows if r['analysis'] == 'setup' and r['edge'] == edge)+1
                        require(early <= late, 'Reversed GPIO Q interval')
                        values.extend((early, late))
                    roles.append(values)
                else:
                    roles.append([max(r['value_ps'] for r in rows if r['analysis'] == a)+3
                                  for a in ('setup', 'hold')])
            corners.append(roles)
        result[pin] = corners
    return result


def load(root=ROOT, reader=None):
    root = Path(root)
    manifest = json.loads((root/'manifest.json').read_text())
    require(manifest['schema'] == 1 and manifest['device'] == '5CSEBA6U23I7', 'Unsupported reference')
    require(manifest['corners'] == list(CORNERS), 'Changed corner identity')
    require(manifest['q_rounding_guard_ps'] == 1 and manifest['constraint_rounding_guard_ps'] == 3,
            'Changed report rounding policy')
    expected_reports = {f'native/{c}/{f}-setup.rpt' for c in CORNERS for f in ('input', 'output')} | {
        f'native-rf/{c}/{f}-{e}-{a}.rpt' for c in CORNERS for f in ('register', 'fabric')
        for e in ('rise', 'fall') for a in ('setup', 'hold')} | {
        f'native-clock/{c}/{e}-{a}.rpt' for c in CORNERS for e in ('rise', 'fall') for a in ('max', 'min')}
    require(set(manifest['reports_sha256']) == expected_reports, 'Incomplete raw report inventory')
    require(set(manifest['source_sha256']) == set(SOURCES), 'Incomplete fitted source inventory')
    for name, sha in manifest['source_sha256'].items():
        require(digest(root/'fixture'/name) == sha, 'Changed source input: '+name)
    require(digest(root/'reference.json.gz') == manifest['reference_gzip_sha256'], 'Changed reference archive')
    require(digest(root/'fixture/fitted.rbf.gz') == manifest['rbf_gzip_sha256'], 'Changed fitted RBF archive')
    rbf_bytes = gzip.decompress((root/'fixture/fitted.rbf.gz').read_bytes())
    require(hashlib.sha256(rbf_bytes).hexdigest() == manifest['rbf_sha256'], 'Changed fitted RBF')
    data = json.loads(gzip.decompress((root/'reference.json.gz').read_bytes()))
    parsed, clocks, reports = [], [], {}
    for name, sha in manifest['reports_sha256'].items():
        blob = gzip.decompress((root/'reports'/(name+'.gz')).read_bytes())
        require(hashlib.sha256(blob).hexdigest() == sha, 'Changed raw report: '+name)
        reports[name] = blob.decode('latin1')
        validate_report(name, reports[name])
        if name.startswith('native-rf/'):
            parsed.extend(row for path in paths(blob.decode('latin1'))
                          if (row := extract(path, data['binding'], data['configuration'], name)) is not None)
        elif name.startswith('native-clock/'):
            clocks.extend(row for path in paths(blob.decode('latin1'))
                          if (row := extract_clock(path, data['binding'], data['configuration'], name)) is not None)
    require(bindings((root/'fixture/top.qsf').read_text(), reports) == data['binding'],
            'GPIO register bindings differ from fitted pad reports')
    require(parsed == data['observations'], 'Parsed observations differ from retained raw reports')
    require(clocks == data['clock_observations'], 'Parsed clock prefixes differ from retained raw reports')
    require(len(parsed) == manifest['observations'] and len(data['binding']) == manifest['register_roles'] and
            len(clocks) == manifest['clock_observations'] and len(data['configuration']) == manifest['package_pins'],
            'Changed reference coverage')
    validate(data)
    if reader:
        with tempfile.TemporaryDirectory(prefix='gpio-core-reference-') as directory:
            rbf = Path(directory)/'fitted.rbf'
            rbf.write_bytes(rbf_bytes)
            require(configuration(reader, rbf, sorted(data['configuration'])) == data['configuration'],
                    'Physical selectors or routing identities changed')
    return manifest, data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config-reader', type=Path)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    manifest, data = load(args.root, args.config_reader)
    print(len(data['binding']), 'physical GPIO register roles;', len(data['observations']),
          'native-cut observations;', len(envelopes(data)), 'package pins; no hardware qualification')
