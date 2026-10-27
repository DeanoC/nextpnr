#!/usr/bin/env python3
"""Generate guarded native-cut FF envelopes from immutable fitted references."""
import argparse
import gzip
import json
import math
from pathlib import Path
import re
import tempfile
from reference import CORNERS, SOURCE_FILES, check_config, digest, fixture, keys, source_digest

# Keep these bits identical to ff_core_reference_data.h / ff_core.cc.
MLAB, CLOCK_INV, ENABLED, ENABLE_INV = 1, 2, 16, 32
SYNC_CLEAR, SYNC_INV, ASYNC_CLEAR, CLKB = 64, 128, 256, 512
SYNC_DIN3 = 1024
ROLES = ('Q', 'd', 'asdata_e', 'asdata_f', 'ena', 'sclr')
ROOT = Path(__file__).resolve().parent


def control(source, cell, port):
    body = re.search(r'\b' + re.escape(cell) + r'\s*\((.*?)\);', source, re.S)
    assert body, cell
    value = re.search(r'\.' + port + r'\(\s*([^()]+?)\s*\)', body[1])
    assert value, (cell, port)
    return value[1]


def mode_key(config, site, source):
    """Use fitted physical settings, independently checked against source intent."""
    m, ci = config['settings'], config['clock_index']
    assert ci in (0, 1, 2) and config['clock_source'] in ('CLKA', 'CLKB')
    enabled = m[f'EN{ci}_EN'] == '1'
    inverted = m[f'CLK{ci}_INV'] == '1'
    assert inverted == site['clock_inverted']
    assert enabled == site.get('dynamic_enable', 'enable_input' in site)
    if enabled:
        assert (m[f'EN{ci}_NINV'] == '1') == site['enable_inverted']
    half_disable = m['TSCLR_DIS' if site['ff_index']//2 == 0 else 'BSCLR_DIS']
    sync = m['SCLR_DIS'] == '0' and half_disable == '0'
    assert sync == site.get('synchronous_clear_enabled', 'clear_input' in site)
    if sync:
        assert (m['SCLR_INV'] == '1') == site['clear_inverted']
        assert m['SCLR_SEL'] in ('DIN1', 'DIN3')
    for port, value in [('prn', "1'b1"), ('sload', "1'b0"), ('aload', "1'b0")]:
        assert control(source, site['cell'], port) == value
    clear = control(source, site['cell'], 'clrn')
    assert clear in ('reset_n', "1'b1")
    return (int(site['block'] == 'MLAB') | (CLOCK_INV if inverted else 0) | (ci << 2) |
            (ENABLED if enabled else 0) | (ENABLE_INV if enabled and m[f'EN{ci}_NINV'] == '1' else 0) |
            (SYNC_CLEAR if sync else 0) | (SYNC_INV if sync and m['SCLR_INV'] == '1' else 0) |
            (ASYNC_CLEAR if clear == 'reset_n' else 0) | (CLKB if config['clock_source'] == 'CLKB' else 0) |
            (SYNC_DIN3 if sync and m['SCLR_SEL'] == 'DIN3' else 0))


def load_reference(root=ROOT, config_reader=None):
    root = Path(root)
    manifest = json.loads((root/'native-manifest.json').read_text())
    assert manifest['schema'] == 1 and manifest['device'] == '5CSEBA6U23I7'
    assert manifest['corners'] == list(CORNERS) and manifest['public_native_corner'] == 'slow100'
    assert manifest['q_rounding_guard_ps'] == 1 and manifest['native_constraint_rounding_guard_ps'] == 3
    assert manifest['parser_sha256'] == digest(root/'reference.py')
    for witness in manifest.get('raw_report_witnesses', []):
        path = root/witness['path']
        assert digest(path) == witness['gzip_sha256']
        import hashlib
        assert hashlib.sha256(gzip.decompress(path.read_bytes())).hexdigest() == witness['raw_sha256']
    archive = root/'native-reference.json.gz'
    assert digest(archive) == manifest['reference_gzip_sha256']
    data = json.loads(gzip.decompress(archive.read_bytes()))
    assert set(data['cases']) == set(manifest['cases'])
    for name, case in manifest['cases'].items():
        directory = root/'fixtures'/name
        assert all(source_digest(directory/n) == h for n, h in case['source_sha256'].items())
        assert digest(directory/'fixture.json.gz') == case['fixture_sha256']
        assert digest(directory/'fitted.rbf.gz') == case['rbf_gzip_sha256']
        spec = fixture(root, name)
        source = (directory/'top.sv').read_text()
        sites = {s['cell']: s for s in spec['sites']}
        configs = data['cases'][name]['configuration']
        assert len(configs) == len(sites) and {c['cell'] for c in configs} == set(sites)
        for config in configs:
            assert config['mode_key'] == mode_key(config, sites[config['cell']], source)
        rows = data['cases'][name]['observations']
        expected = {(c, r, a, e, s['cell']) for c, r, a, e in keys(spec) for s in spec['sites']
                    if r in s.get('report_roles', spec['report_roles'])}
        actual = {(r['corner'], r['role'], r['analysis'], r['edge'], r['cell']) for r in rows}
        assert len(rows) == len(actual) and actual == expected
        for row in rows:
            site = sites[row['cell']]
            assert row['location'] == site['location'] and row['block'] == site['block']
            assert row['clock_inverted'] == site['clock_inverted']
            if row['role'] != 'Q':
                setup = row['analysis'] in ('setup', 'recovery')
                value = (row['native_data_local_ns'] - row['native_clock_local_ns'] - row['intrinsic_adjustment_ns']
                         if setup else row['native_clock_local_ns'] + row['intrinsic_adjustment_ns'] - row['native_data_local_ns'])
                assert math.isfinite(value) and abs(value-row['effective_native_constraint_ns']) < .0000006
        if config_reader:
            with tempfile.TemporaryDirectory(prefix='ff-native-reference-') as tmp:
                rbf = Path(tmp)/'fitted.rbf'
                rbf.write_bytes(gzip.decompress((directory/'fitted.rbf.gz').read_bytes()))
                assert digest(rbf) == case['rbf_sha256']
                checked = check_config(config_reader, rbf, spec, rows)
                assert [{k:v for k,v in c.items() if k != 'mode_key'} for c in configs] == checked
    return manifest, data


def picoseconds(value):
    result = round(value*1000)
    assert math.isfinite(value) and abs(value*1000-result) < .00001
    return result


def envelopes(data):
    samples = {}
    for case in data['cases'].values():
        configs = {c['cell']: c for c in case['configuration']}
        for row in case['observations']:
            if row['role'] == 'ACLR':
                continue  # Clear recovery/removal remains a separate opt-in.
            role = 'Q' if row['role'] == 'Q' else row['physical_pin']
            config = configs[row['cell']]
            if role == 'asdata':
                selectors = [v for k,v in config['settings'].items() if k in ('TEF_SEL', 'BEF_SEL')]
                assert len(selectors) == 1 and selectors[0] in ('E', 'F')
                role = 'asdata_' + selectors[0].lower()
            assert role in ROLES
            samples.setdefault((config['mode_key'], row['corner'], role), []).append(row)
    result = {}
    for mode in sorted({k[0] for k in samples}):
        corners = []
        for corner in CORNERS:
            roles = []
            for role in ROLES:
                rows = samples.get((mode, corner, role), [])
                if not rows:
                    roles.append(None)
                    continue
                assert {(r['analysis'],r['edge']) for r in rows} == {
                    (a,e) for a in ('setup','hold') for e in ('rise','fall')}
                if role == 'Q':
                    values = []
                    for edge in ('rise','fall'):
                        early = min(picoseconds(r['clock_anchor_to_q_ns']) for r in rows if r['analysis']=='hold' and r['edge']==edge)-1
                        late = max(picoseconds(r['clock_anchor_to_q_ns']) for r in rows if r['analysis']=='setup' and r['edge']==edge)+1
                        assert early <= late
                        values.extend((early,late))
                else:
                    # Five rounded report quantities enter the native-cut
                    # equation; 5 * 0.5 ps needs an outward 3 ps guard.
                    values = [max(picoseconds(r['effective_native_constraint_ns']) for r in rows if r['analysis']==analysis)+3
                              for analysis in ('setup','hold')]
                roles.append(values)
            assert roles[0] is not None
            corners.append(roles)
        result[mode] = corners
    return result


def header(manifest, data):
    lines = ['// Generated by mistral/tests/ff-core/model_reference.py; do not edit.',
             '// Native local FF references, not whole-chip or physical signoff.', '#pragma once',
             'namespace ff_core_reference {',
             'enum ModeBits : unsigned { Mlab=1, ClockInv=2, Enabled=16, EnableInv=32,',
             '                           SyncClear=64, SyncInv=128, AsyncClear=256, Clkb=512, SyncDin3=1024 };',
             'struct Bounds { int qr_min, qr_max, qf_min, qf_max; int setup[5], hold[5]; unsigned valid; };',
             'struct Mode { unsigned key; Bounds corners[4]; };',
             'inline constexpr const char reference_sha256[] = "'+manifest['reference_gzip_sha256']+'";',
             'inline constexpr Mode modes[] = {']
    for mode, corners in envelopes(data).items():
        lines.append('    {'+str(mode)+', {')
        for roles in corners:
            valid = sum(1 << i for i,r in enumerate(roles) if r is not None)
            q = ','.join(map(str,roles[0]))
            setup = ','.join(str(r[0]) if r else '0' for r in roles[1:])
            hold = ','.join(str(r[1]) if r else '0' for r in roles[1:])
            lines.append('        {'+q+', {'+setup+'}, {'+hold+'}, '+str(valid)+'},')
        lines.append('    }},')
    return '\n'.join([*lines, '};', '} // namespace ff_core_reference', ''])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--config-reader', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT.parent.parent/'ff_core_reference_data.h')
    args = parser.parse_args()
    manifest, data = load_reference(config_reader=args.config_reader)
    text = header(manifest, data)
    if args.check:
        assert args.output.read_text() == text
    else:
        args.output.write_text(text)
    print(len(data['cases']), 'source-bound FF cases;', len(envelopes(data)), 'physical mode classes')
