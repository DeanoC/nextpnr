#!/usr/bin/env python3
"""Route and replay PLLs with each configured output unused in turn.

Checks counter/pin-map preservation, not board timing or hardware acceptance.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixture = Path(__file__).resolve().parent / 'fixtures/general/mister3'

    def run(command, directory, label):
        with (directory / (label + '.log')).open('w') as log:
            subprocess.run([str(x) for x in command], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=180)

    for unused in (None, 0, 1, 2):
        d = out / ('all-used' if unused is None else f'unused-{unused}')
        d.mkdir()
        source = (fixture / 'top.v').read_text()
        if unused is not None:
            source = source.replace(f"    always @(posedge core_clk[{unused}]) c{unused} <= c{unused} + 1'b1;\n", '')
            source = source.replace(f' ^ c{unused}[3]', '')
        (d / 'top.v').write_text(source)
        run([args.yosys.resolve(), '-p', f'read_verilog {d / "top.v"}; '
             f'synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {d / "synth.json"}'], d, 'synth')
        base = [args.nextpnr.resolve(), '--device', '5CSEBA6U23I7', '--timing-allow-fail', '--compress-rbf']
        run(base + ['--json', d / 'synth.json', '--qsf', fixture / 'pins.qsf', '--sdc', fixture / 'clocks.sdc',
                    '--write', d / 'routed.json', '--rbf', d / 'original.rbf'], d, 'route')
        before = json.loads((d / 'routed.json').read_text())['modules']['top']
        pll = before['cells']['pll_core']
        counters = pll['attributes']['MISTRAL_PLL_COUNTERS'].split(',')
        assert len(counters) == 3 and len(set(counters)) == 3, counters
        # Unused counters keep configuration but do not consume clock lanes.
        assert sum(c['type'] == 'MISTRAL_CLKBUF' for c in before['cells'].values()) == (5 if unused is None else 4)
        run(base + ['--json', d / 'routed.json', '--no-pack', '--no-place', '--no-route',
                    '--write', d / 'reloaded.json', '--rbf', d / 'reloaded.rbf'], d, 'reload')
        after = json.loads((d / 'reloaded.json').read_text())['modules']['top']['cells']['pll_core']
        for key in ('MISTRAL_PLL_COUNTERS', 'FES_PINMAP_V1'):
            assert pll['attributes'][key] == after['attributes'][key], key
        assert (d / 'original.rbf').read_bytes() == (d / 'reloaded.rbf').read_bytes()
        if unused is not None:
            # JSON can also omit the unused connection entirely. Exercise
            # that path separately from a driven net with zero consumers.
            disconnected = json.loads((d / 'synth.json').read_text())
            disconnected['modules']['top']['cells']['pll_core']['connections']['outclk'][unused] = 'x'
            (d / 'disconnected.json').write_text(json.dumps(disconnected))
            run(base + ['--json', d / 'disconnected.json', '--qsf', fixture / 'pins.qsf',
                        '--pack-only', '--write', d / 'disconnected-packed.json'], d, 'disconnected')
            packed = json.loads((d / 'disconnected-packed.json').read_text())['modules']['top']
            assert packed['cells']['pll_core']['attributes']['MISTRAL_PLL_COUNTERS'] == pll['attributes']['MISTRAL_PLL_COUNTERS']
            assert sum(c['type'] == 'MISTRAL_CLKBUF' for c in packed['cells'].values()) == 4
    print('PASS: all-used and three unused-output routes preserve counters, lanes and replayed RBF')


if __name__ == '__main__':
    main()
