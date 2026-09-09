#!/usr/bin/env python3
"""Route independent-clock M10Ks and check their clock configuration and timing."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'mistral-cv', 'qsf', 'sdc', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_suffix('.v').resolve()
    for width in (20, 40):
        directory = out / str(width)
        directory.mkdir(exist_ok=True)
        script = directory / 'synth.ys'
        script.write_text(f'read_verilog {source}\nchparam -set WIDTH {width} top\n'
                          'synth_intel_alm -nolutram -nodsp -top top\n'
                          'select -assert-count 1 t:MISTRAL_M10K\n'
                          f'write_json {directory / "synth.json"}\n')
        with (directory / 'synth.log').open('w') as log:
            subprocess.run([str(args.yosys.resolve()), '-Q', '-T', '-s', str(script)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        design = json.loads((directory / 'synth.json').read_text())
        name, ram = next((n, c) for n, c in design['modules']['top']['cells'].items()
                         if c['type'] == 'MISTRAL_M10K')
        assert int(ram['parameters']['CFG_DUAL_CLOCK'], 2) == 1
        assert int(ram['parameters']['CFG_DBITS'], 2) == width
        assert ram['connections']['CLK1'] != ram['connections']['CLK2']
        common = [str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                  '--qsf', str(args.qsf.resolve()), '--sdc', str(args.sdc.resolve())]
        with (directory / 'route.log').open('w') as log:
            subprocess.run(common + ['--json', str(directory / 'synth.json'), '--compress-rbf',
                           '--rbf', str(directory / 'top.rbf'), '--write', str(directory / 'routed.json'),
                           '--report', str(directory / 'timing.json')],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        assert (directory / 'top.rbf').stat().st_size > 0
        report = json.loads((directory / 'timing.json').read_text())
        for resource in ('MISTRAL_M10K', 'altera_pll', 'cyclonev_hps_interface_mpu_general_purpose'):
            assert report['utilization'][resource]['used'] == 1
        for resource in ('MISTRAL_MUL9X9', 'MISTRAL_MUL18X18', 'MISTRAL_MUL27X27'):
            assert report['utilization'][resource]['used'] == 0
        assert all(c['achieved'] >= c['constraint'] == 50 for c in report['fmax'].values())
        assert report['fmax']
        assert any(p['from'] == 'posedge read_clock' and p['max_delay'] == 40 and
                   any(a['type'] == 'clk-to-q' and a['from']['cell'] == name and
                       a['from']['port'].startswith('B1DATA[') for a in p['path'])
                   for p in report['critical_paths']), 'Missing 25 MHz read-clock timing arc'
        subprocess.run([str(args.mistral_cv.resolve()), 'decomp', '5CSEBA6U23I7',
                        str(directory / 'top.rbf'), str(directory / 'top.bt')], check=True)
        bt = (directory / 'top.bt').read_text()
        routed = json.loads((directory / 'routed.json').read_text())['modules']['top']['cells'][name]
        _, x, y, _ = routed['attributes']['NEXTPNR_BEL'].split('.')
        prefix = f'M10K.{int(x):03d}.{int(y):03d}'
        fields = dict(re.findall(r'^s ' + re.escape(prefix) + r':(\S+) (\S+)$', bt, re.M))
        for field, value in {'TOP_CLK_SEL': '1', 'BOT_CLK_SEL': '1',
                             'BOT_1_CORECLK_SEL': '1', 'BOT_1_OUTCLK_SEL': '1',
                             'BOT_1_INCLK_SEL': '0' if width == 40 else '1',
                             'BOT_CLK_INV': '0', 'BOT_CORECLK_SEL': '1', 'BOT_INCLK_SEL': '1'}.items():
            assert fields.get(field, '0') == value, (field, fields.get(field))
        for pin in (0, 1):
            assert re.search(r'^r \S+ ' + re.escape(prefix) + rf':CLKIN\.{pin}$', bt, re.M)
        for label, missing, flag, message in (
            ('missing-read', 'CLK2', True, 'requires a connected CLK2 clock'),
            ('missing-write', 'CLK1', True, 'requires a connected CLK1 clock'),
            ('unselected-read', None, False, 'CLK2 requires CFG_DUAL_CLOCK=1')):
            invalid = copy.deepcopy(design)
            cell = invalid['modules']['top']['cells'][name]
            if missing:
                cell['connections'][missing] = []
                if missing == 'CLK2':
                    # Keep the gated clock used so its own validation does not
                    # precede the intentionally malformed M10K diagnostic.
                    cell['connections']['CLK1'] = ram['connections']['CLK2']
            if not flag:
                cell['parameters'].pop('CFG_DUAL_CLOCK')
            path = directory / f'{label}.json'
            path.write_text(json.dumps(invalid))
            result = subprocess.run(common + ['--json', str(path)], capture_output=True, text=True)
            (directory / f'{label}.log').write_text(result.stdout + result.stderr)
            assert result.returncode != 0 and message in result.stdout + result.stderr, label
        print(f'PASS: width={width}, one M10K, independent clock routing/timing, configuration, diagnostics')


if __name__ == '__main__':
    main()
