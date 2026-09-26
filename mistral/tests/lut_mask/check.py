#!/usr/bin/env python3
"""Check emitted FF route-through masks and initialized MLAB storage, not just routing."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def run(command, log):
    with log.open('w') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'mistral-cv', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([str(args.yosys.resolve()), '-p',
         f'read_verilog {here / "top.v"}; synth_intel_alm -nobram -nodsp -top top; write_json {out / "synth.json"}'], out / 'synth.log')
    run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7', '--json', str(out / 'synth.json'),
         '--qsf', str(here / 'pins.qsf'), '--seed', '1', '--write', str(out / 'routed.json'),
         '--rbf', str(out / 'core.rbf'), '--compress-rbf'], out / 'route.log')
    run([str(args.mistral_cv.resolve()), 'decomp', '5CSEBA6U23I7', str(out / 'core.rbf'), str(out / 'core.bt')], out / 'decompile.log')
    cells = json.loads((out / 'routed.json').read_text())['modules']['top']['cells']
    settings = dict(re.findall(r'^s (\S+) (\S+)$', (out / 'core.bt').read_text(), re.M))
    def half(cell):
        _, x, y, z = cell['attributes']['NEXTPNR_BEL'].split('.')
        z = int(z)
        assert z % 6 in (0, 1)
        kind = 'MLAB' if 'MCOMB' in cell['attributes']['NEXTPNR_BEL'] else 'LAB'
        value = int(settings.get(f'{kind}.{int(x):03d}.{int(y):03d}:LUT_MASK.{z // 6}', '0').replace('.', ''), 16)
        return kind, (value >> (32 * (z % 6))) & 0xffffffff
    permutation = (0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
                   2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18)
    buffers = memories = inversions = parity = 0
    buffer_halves = set()
    for name, cell in cells.items():
        if cell['type'] == 'MISTRAL_BUF':
            assert '$ROUTETHRU' in name, name
            assert any(c['type'] == 'MISTRAL_FF' and c['connections'].get('DATAIN') == cell['connections']['Q'] for c in cells.values()), name
            kind, actual = half(cell)
            expected = 0xf0f0f0f0
            if kind == 'MLAB':
                expected = sum(((expected >> i) & 1) << p for i, p in enumerate(permutation))
            assert actual == expected, (name, kind, hex(actual), hex(expected))
            buffers += 1
            buffer_halves.add(int(cell["attributes"]["NEXTPNR_BEL"].split(".")[-1]) % 6)
        elif cell['type'] == 'MISTRAL_NOT':
            kind, actual = half(cell)
            # The half-specific C/D input is truth-table address bit2.
            expected = 0x0f0f0f0f
            if kind == 'MLAB':
                expected = sum(((expected >> i) & 1) << p for i, p in enumerate(permutation))
            assert actual == expected, (name, hex(actual), hex(expected))
            inversions += 1
        elif name.startswith('parity_') and cell['type'] == 'MISTRAL_ALUT2':
            # Fixture XOR uses C/D and E0/E1 (address bits2/3). CRAM is active-low.
            assert half(cell) == ('LAB', 0xf00ff00f), (name, half(cell))
            parity += 1
        elif cell['type'] == 'MISTRAL_MLAB':
            kind, actual = half(cell)
            assert kind == 'MLAB'
            initial = int(cell['parameters']['INIT'], 2)
            for address in range(32):
                assert (1 ^ ((actual >> permutation[31-address]) & 1)) == ((initial >> address) & 1)
            memories += 1
    assert buffer_halves == {0, 1}, buffer_halves
    assert memories > 0 and inversions > 0 and parity == 1, (memories, inversions, parity)
    print(f'PASS: {buffers} routed FF DATAIN buffers in both halves; {inversions} inversions; XOR; {memories} initialized MLAB masks')


if __name__ == '__main__':
    main()
