#!/usr/bin/env python3
"""Exercise MISTRAL_MLAB INIT using the synthesized misteross 040 fixture."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def contents(address):
    return ((address * 73) ^ (address >> 1) ^ 0xA6) & 255


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('nextpnr', 'mistral-cv', 'fixture', 'qsf', 'sdc', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--pattern', choices=('data', 'zero', 'ones', 'omitted', 'unknown'), default='data')
    parser.add_argument('--negative', action='store_true', help='also check malformed INIT diagnostics')
    parser.add_argument('--width', choices=(1, 7, 8), type=int, default=8)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    design = json.loads(args.fixture.read_text())
    mlabs = {name: cell for name, cell in design['modules']['top']['cells'].items()
             if cell['type'] == 'MISTRAL_MLAB'}
    assert len(mlabs) == 8, len(mlabs)
    for name, cell in list(mlabs.items()):
        match = re.fullmatch(r'storage\.stored\.(\d)\.0\.0', name)
        assert match, name
        bit = int(match[1])
        if bit >= args.width:
            output = cell['connections']['B1DATA'][0]
            del design['modules']['top']['cells'][name]
            del mlabs[name]
            for other in design['modules']['top']['cells'].values():
                for port, bits in other['connections'].items():
                    other['connections'][port] = ['0' if b == output else b for b in bits]
            continue
        init = sum(((contents(addr) >> bit) & 1) << addr for addr in range(32))
        if args.pattern == 'zero':
            init = 0
        elif args.pattern == 'ones':
            init = 0xffffffff
        cell['parameters'].pop('INIT', None)
        if args.pattern != 'omitted':
            cell['parameters']['INIT'] = '10xz' * 8 if args.pattern == 'unknown' else f'{init:032b}'
    fixture = out / 'synth.json'
    fixture.write_text(json.dumps(design))
    with (out / 'route.log').open('w') as log:
        subprocess.run([str(args.nextpnr.resolve()), '--json', str(fixture),
                        '--device', '5CSEBA6U23I7', '--qsf', str(args.qsf.resolve()),
                        '--sdc', str(args.sdc.resolve()), '--freq', '50', '--compress-rbf',
                        '--rbf', str(out / 'top.rbf'), '--write', str(out / 'routed.json'),
                        '--report', str(out / 'timing.json')], stdout=log, stderr=subprocess.STDOUT, check=True)
    report = json.loads((out / 'timing.json').read_text())
    util = report['utilization']
    assert util['cyclonev_hps_interface_mpu_general_purpose']['used'] == 1
    for resource in ('MISTRAL_M10K', 'MISTRAL_MUL9X9', 'MISTRAL_MUL18X18', 'MISTRAL_MUL27X27', 'altera_pll'):
        assert util[resource]['used'] == 0, (resource, util[resource])
    clock = report['fmax']['storage.FPGA_CLK1_50']
    assert clock['constraint'] == 50 and clock['achieved'] >= 50, clock
    subprocess.run([str(args.mistral_cv.resolve()), 'decomp', '5CSEBA6U23I7',
                    str(out / 'top.rbf'), str(out / 'top.bt')], check=True)
    bt = (out / 'top.bt').read_text()
    routed = json.loads((out / 'routed.json').read_text())['modules']['top']['cells']
    # Physical CRAM bit order independently checked against Quartus's RAM oracle.
    permutation = (0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
                   2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18)
    occupied = {}
    for name, cell in mlabs.items():
        _, x, y, z = routed[name]['attributes']['NEXTPNR_BEL'].split('.')
        key = f'MLAB.{int(x):03d}.{int(y):03d}:LUT_MASK.{int(z) // 6}'
        lane = int(z) % 6
        assert lane in (0, 1), (name, z)
        occupied.setdefault(key, {})[lane] = cell
    for key, lanes in occupied.items():
        match = re.search(r'^s ' + re.escape(key) + r' (\S+)$', bt, re.M)
        # An all-zero physical mask can be omitted by the decompiler.
        actual = int(match[1].replace('.', ''), 16) if match else 0
        for lane in range(2):
            init = lanes.get(lane, {}).get('parameters', {}).get('INIT', '0' * 32)
            init = int(init.replace('x', '0').replace('z', '0'), 2)
            for addr in range(32):
                physical = permutation[31 - addr] + lane * 32
                value = 1 ^ ((actual >> physical) & 1)
                assert value == ((init >> addr) & 1), (key, lane, addr, 'INIT mismatch')
    if args.negative:
        for label, invalid in (('wide', '1' * 33), ('string', 'invalid')):
            next(iter(mlabs.values()))['parameters']['INIT'] = invalid
            bad = out / f'{label}.json'
            bad.write_text(json.dumps(design))
            result = subprocess.run([str(args.nextpnr.resolve()), '--json', str(bad),
                                     '--device', '5CSEBA6U23I7', '--qsf', str(args.qsf.resolve())],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            (out / f'{label}.log').write_text(result.stdout)
            assert result.returncode != 0, label
            assert 'INIT must be a numeric value of at most 32 bits' in result.stdout, result.stdout
    print('PASS: initialized MLAB configuration', report['fmax'])


if __name__ == '__main__':
    main()
