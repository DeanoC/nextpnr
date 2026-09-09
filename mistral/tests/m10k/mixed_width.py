#!/usr/bin/env python3
"""Infer and route mixed-width M10K SDP RAMs in both width directions."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


def run(command, log, timeout=None):
    with log.open('w') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=timeout)


def diagnostics(args, design, name, out):
    cases = (
        ('write-geometry', {'CFG_DBITS': 30}, None, 'mixed widths require'),
        ('read-geometry', {'CFG_RD_ABITS': 8}, None, 'mixed widths require'),
        ('single-clock', {'CFG_DUAL_CLOCK': 0}, None, 'both clocks'),
        ('missing-clock', {}, 'CLK1', 'both clocks'),
        ('byte-mask', {'CFG_BYTE_ENABLE': 1}, None, 'byte enables are not supported'),
        ('missing-mode', {'CFG_MIXED_WIDTH': 0}, None, 'separate read geometry requires'),
    )
    for label, params, disconnect, message in cases:
        bad = copy.deepcopy(design)
        cell = bad['modules']['top']['cells'][name]
        cell['parameters'].update({k: format(v, '032b') for k, v in params.items()})
        if disconnect:
            cell['connections'][disconnect] = []
        fixture = out / (label + '.json')
        fixture.write_text(json.dumps(bad))
        result = subprocess.run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7',
                                 '--qsf', str(args.qsf.resolve()), '--json', str(fixture)], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        (out / (label + '.log')).write_text(result.stdout)
        assert result.returncode != 0 and message in result.stdout, (label, result.stdout[-2000:])
    print('PASS: six invalid mixed-width configurations rejected', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'mistral-cv', 'qsf', 'sdc', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--case', action='append', help='Run only this uN-wN-rN case (repeatable)')
    parser.add_argument('--route-timeout', type=float, default=120, help='Seconds allowed per routing run')
    args = parser.parse_args()
    if args.route_timeout <= 0:
        parser.error('--route-timeout must be positive')
    out = args.output.resolve()
    source = Path(__file__).with_suffix('.v').resolve()
    for unit, w, r in ((10,4,1), (10,1,4), (10,2,1), (10,1,2),
                       (10,4,2), (10,2,4), (8,4,1), (8,1,4),
                       (10,1,1), (10,2,2), (10,4,4)):
        if args.case and f'u{unit}-w{w}-r{r}' not in args.case:
            continue
        case = out / f'u{unit}-w{w}-r{r}'
        case.mkdir(parents=True, exist_ok=True)
        script = case / 'synth.ys'
        script.write_text(f'read_verilog {source}\n'
                          f'chparam -set UNIT {unit} -set WLANES {w} -set RLANES {r} top\n'
                          'synth_intel_alm -nolutram -nodsp -top top\n'
                          'select -assert-count 1 t:MISTRAL_M10K\n'
                          f'write_json {case / "synth.json"}\n')
        run([str(args.yosys.resolve()), '-Q', '-T', '-s', str(script)], case / 'synth.log')
        design = json.loads((case / 'synth.json').read_text())
        name, cell = next((n,c) for n,c in design['modules']['top']['cells'].items()
                          if c['type'] == 'MISTRAL_M10K')
        params = {k:int(v,2) for k,v in cell['parameters'].items() if k != 'INIT'}
        assert params['CFG_MIXED_WIDTH'] == params['CFG_DUAL_CLOCK'] == 1
        assert params['CFG_DBITS'] == w*10 and params['CFG_RD_DBITS'] == r*10, params
        assert params['CFG_ABITS'] == 10-(w.bit_length()-1)
        assert params['CFG_RD_ABITS'] == 10-(r.bit_length()-1)
        assert len(cell['connections']['A1DATA']) == w*10
        assert len(cell['connections']['B1DATA']) == r*10
        if (unit, w, r) == (10, 4, 1):
            diagnostics(args, design, name, case)
        run([str(args.nextpnr.resolve()), '--device', '5CSEBA6U23I7', '--freq', '50',
             '--qsf', str(args.qsf.resolve()), '--sdc', str(args.sdc.resolve()),
             '--json', str(case/'synth.json'), '--compress-rbf', '--rbf', str(case/'top.rbf'),
             '--write', str(case/'routed.json'), '--report', str(case/'timing.json')],
            case/'route.log', timeout=args.route_timeout)
        report = json.loads((case/'timing.json').read_text())
        for resource in ('MISTRAL_M10K','altera_pll','cyclonev_hps_interface_mpu_general_purpose'):
            assert report['utilization'][resource]['used'] == 1
        assert report['fmax'] and all(c['achieved'] >= c['constraint'] == 50 for c in report['fmax'].values())
        assert (case/'top.rbf').stat().st_size > 0
        run([str(args.mistral_cv.resolve()), 'decomp', '5CSEBA6U23I7',
             str(case/'top.rbf'), str(case/'top.bt')], case/'decomp.log')
        bt = (case/'top.bt').read_text()
        routed = json.loads((case/'routed.json').read_text())['modules']['top']['cells'][name]
        _,x,y,_ = routed['attributes']['NEXTPNR_BEL'].split('.')
        site = f'M10K.{int(x):03d}.{int(y):03d}'
        fields = dict(re.findall(r'^s '+re.escape(site)+r':(\S+) (\S+)$',bt,re.M))
        for field,value in {'A_DATA_WIDTH':str(w*10), 'B_DATA_WIDTH':str(r*10),
                            'TOP_CLK_SEL':'1', 'BOT_CLK_SEL':'1', 'TOP_CE0_SEL':'1',
                            'TOP_CORECLK_SEL':'1', 'BOT_CORECLK_SEL':'1', 'BOT_INCLK_SEL':'1',
                            'BOT_1_CORECLK_SEL':'1','BOT_1_OUTCLK_SEL':'1',
                            'BOT_1_INCLK_SEL':'0' if 4 in (w,r) else '1',
                            'A_DATA_FLOW_THRU':'0' if 4 in (w,r) else '1',
                            'B_DATA_FLOW_THRU':'0' if 4 in (w,r) else '1',
                            'TOP_W_INV':'0','TOP_W_SEL':'0','BOT_W_SEL':'0'}.items():
            default = '40' if field=='A_DATA_WIDTH' else '0'
            assert fields.get(field,default) == value, (field,fields.get(field),value)
        for pin in ('CLKIN.0','CLKIN.1','WREN.0','ENABLE.0','ENABLE.1'):
            assert re.search(r'^r \S+ '+re.escape(site+':'+pin)+r'$',bt,re.M),pin
        print(f'PASS: {case.name}: one mixed M10K, port geometry, routing, settings, RBF and 50 MHz',flush=True)


if __name__ == '__main__':
    main()
