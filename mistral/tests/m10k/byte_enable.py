#!/usr/bin/env python3
"""Route and inspect a 20-bit M10K with two physical byte-enable lanes."""
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
    for name in ('yosys', 'nextpnr', 'mistral-cv', 'qsf', 'sdc', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_suffix('.v').resolve()
    script = out / 'synth.ys'
    script.write_text(f'read_verilog {source}\n'
                      'synth_intel_alm -nolutram -nodsp -top top\n'
                      'select -assert-count 1 t:MISTRAL_M10K\n'
                      f'write_json {out / "synth.json"}\n')
    run([str(args.yosys.resolve()), '-Q', '-T', '-s', str(script)], out / 'synth.log')
    design = json.loads((out / 'synth.json').read_text())
    name, ram = next((n, c) for n, c in design['modules']['top']['cells'].items()
                     if c['type'] == 'MISTRAL_M10K')
    assert int(ram['parameters']['CFG_DBITS'], 2) == 20
    assert int(ram['parameters']['CFG_ABITS'], 2) == 9
    assert int(ram['parameters']['CFG_DUAL_CLOCK'], 2) == 1
    assert int(ram['parameters']['CFG_BYTE_ENABLE'], 2) == 1
    assert len(ram['connections']['A1BE']) == 2
    assert len(ram['connections']['A1EN']) == 1
    assert ram['connections']['CLK1'] != ram['connections']['CLK2']

    run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7', '--freq', '50',
         '--qsf', str(args.qsf.resolve()), '--sdc', str(args.sdc.resolve()),
         '--json', str(out / 'synth.json'), '--compress-rbf', '--rbf', str(out / 'top.rbf'),
         '--write', str(out / 'routed.json'), '--report', str(out / 'timing.json')],
        out / 'route.log')
    assert (out / 'top.rbf').stat().st_size > 0
    report = json.loads((out / 'timing.json').read_text())
    for resource in ('MISTRAL_M10K', 'altera_pll', 'cyclonev_hps_interface_mpu_general_purpose'):
        assert report['utilization'][resource]['used'] == 1
    for resource in ('MISTRAL_MUL9X9', 'MISTRAL_MUL18X18', 'MISTRAL_MUL27X27'):
        assert report['utilization'][resource]['used'] == 0
    assert report['fmax']
    assert all(c['achieved'] >= c['constraint'] == 50 for c in report['fmax'].values())

    run([str(args.mistral_cv.resolve()), 'decomp', '5CSEBA6U23I7',
         str(out / 'top.rbf'), str(out / 'top.bt')], out / 'decomp.log')
    bt = (out / 'top.bt').read_text()
    routed = json.loads((out / 'routed.json').read_text())['modules']['top']['cells'][name]
    _, x, y, _ = routed['attributes']['NEXTPNR_BEL'].split('.')
    prefix = f'M10K.{int(x):03d}.{int(y):03d}'
    fields = dict(re.findall(r'^s ' + re.escape(prefix) + r':(\S+) (\S+)$', bt, re.M))
    for field, value in {
        'TOP_CLK_SEL': '1', 'BOT_CLK_SEL': '1', 'BOT_1_CORECLK_SEL': '1',
        'BOT_1_OUTCLK_SEL': '1', 'BOT_1_INCLK_SEL': '1', 'BOT_CLK_INV': '0',
        'BOT_CORECLK_SEL': '1', 'BOT_INCLK_SEL': '1', 'BOT_W_SEL': '0', 'TOP_W_SEL': '0', 'TOP_CE0_SEL': '1',
        'TOP_CORECLK_SEL': '1', 'TOP_W_INV': '0',
    }.items():
        assert fields.get(field, '0') == value, (field, fields.get(field), fields)
    for pin in ('BYTEENABLEA.0', 'BYTEENABLEA.1', 'WREN.0', 'ENABLE.1'):
        assert re.search(r'^r \S+ ' + re.escape(prefix + ':' + pin) + r'$', bt, re.M), pin
    for pin in ('CLKIN.0', 'CLKIN.1'):
        assert re.search(r'^r \S+ ' + re.escape(prefix + ':' + pin) + r'$', bt, re.M), pin
    print('PASS: 20-bit byte-enabled M10K, two byte routes, independent clocks, selectors, RBF, 50 MHz timing')


if __name__ == '__main__':
    main()
