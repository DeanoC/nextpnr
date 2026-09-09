#!/usr/bin/env python3
"""Check common-clock M10K inference and compatibility with legacy primitives."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


def run(command, log):
    with log.open('w') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'mistral-cv', 'qsf', 'sdc', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = Path(__file__).with_name('dual_clock.v').read_text()
    # Reuse the RAM/GP fixture, replacing only its independent clock generator.
    source_text, count = re.subn(
        r'    wire pll_clock, read_clock, locked;.*?(?=    \(\* ramstyle)',
        '    wire read_clock = FPGA_CLK1_50;\n', original, flags=re.S)
    assert count == 1, 'Expected one PLL/gate block in dual_clock.v'
    source = out / 'same_clock.v'
    source.write_text(source_text)
    for width in (20, 40):
        directory = out / str(width)
        directory.mkdir(exist_ok=True)
        script = directory / 'synth.ys'
        script.write_text(f'read_verilog {source}\nchparam -set WIDTH {width} top\n'
                          'synth_intel_alm -nolutram -nodsp -top top\n'
                          'select -assert-count 1 t:MISTRAL_M10K\n'
                          f'write_json {directory / "synth.json"}\n')
        run([str(args.yosys.resolve()), '-Q', '-T', '-s', str(script)], directory / 'synth.log')
        design = json.loads((directory / 'synth.json').read_text())
        name, ram = next((n, c) for n, c in design['modules']['top']['cells'].items()
                         if c['type'] == 'MISTRAL_M10K')
        assert int(ram['parameters']['CFG_DUAL_CLOCK'], 2) == 1
        assert int(ram['parameters']['CFG_DBITS'], 2) == width
        assert ram['connections']['CLK1'] == ram['connections']['CLK2']
        for legacy in (False, True):
            case = directory / ('legacy' if legacy else 'same-clock')
            case.mkdir(exist_ok=True)
            variant = copy.deepcopy(design)
            cell = variant['modules']['top']['cells'][name]
            if legacy:
                cell['parameters'].pop('CFG_DUAL_CLOCK')
                cell['connections'].pop('CLK2')
                cell['port_directions'].pop('CLK2')
            fixture = case / 'synth.json'
            fixture.write_text(json.dumps(variant))
            run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7', '--freq', '50',
                 '--qsf', str(args.qsf.resolve()), '--sdc', str(args.sdc.resolve()),
                 '--json', str(fixture), '--compress-rbf', '--rbf', str(case / 'top.rbf'),
                 '--write', str(case / 'routed.json'), '--report', str(case / 'timing.json')],
                case / 'route.log')
            assert (case / 'top.rbf').stat().st_size > 0
            report = json.loads((case / 'timing.json').read_text())
            for resource in ('MISTRAL_M10K', 'cyclonev_hps_interface_mpu_general_purpose'):
                assert report['utilization'][resource]['used'] == 1
            assert report['utilization']['altera_pll']['used'] == 0
            assert report['fmax']
            assert all(c['achieved'] >= c['constraint'] == 50 for c in report['fmax'].values())
            run([str(args.mistral_cv.resolve()), 'decomp', '5CSEBA6U23I7',
                 str(case / 'top.rbf'), str(case / 'top.bt')], case / 'decomp.log')
            bt = (case / 'top.bt').read_text()
            routed = json.loads((case / 'routed.json').read_text())['modules']['top']['cells'][name]
            _, x, y, _ = routed['attributes']['NEXTPNR_BEL'].split('.')
            prefix = f'M10K.{int(x):03d}.{int(y):03d}'
            fields = dict(re.findall(r'^s ' + re.escape(prefix) + r':(\S+) (\S+)$', bt, re.M))
            assert fields.get('TOP_CLK_INV', '0') == '0', fields
            assert fields.get('BOT_CLK_INV', '0') == ('1' if legacy and width != 40 else '0'), fields
            for field in ('BOT_1_CORECLK_SEL', 'BOT_1_OUTCLK_SEL', 'BOT_1_INCLK_SEL'):
                expected = '0' if legacy or (field == 'BOT_1_INCLK_SEL' and width == 40) else '1'
                assert fields.get(field, '0') == expected, (field, fields)
            assert re.search(r'^r \S+ ' + re.escape(prefix) + r':CLKIN\.0$', bt, re.M)
            read_route = re.search(r'^r \S+ ' + re.escape(prefix) + r':CLKIN\.1$', bt, re.M)
            assert bool(read_route) == (not legacy), (legacy, prefix)
            print(f'PASS: width={width}, {case.name}, clock routing/configuration and 50 MHz timing')


if __name__ == '__main__':
    main()
