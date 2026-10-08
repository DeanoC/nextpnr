#!/usr/bin/env python3
"""Native reservation checkpoint and late FF route-through regression."""
import argparse
import copy
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'top.v'
    source.write_text('''module top(input clk, a, output q_static, q_cart, q_zero);
(* BEL="MISTRAL_FF.24.1.2" *) MISTRAL_FF static_ff (
 .CLK(clk), .DATAIN(a), .Q(q_static), .ACLR(1'b1), .ENA(1'b1),
 .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
(* BEL="MISTRAL_FF.25.1.2", FES_SLOT="cart" *) MISTRAL_FF cart_ff (
 .CLK(clk), .DATAIN(a), .Q(q_cart), .ACLR(1'b1), .ENA(1'b1),
 .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
(* BEL="MISTRAL_FF.24.1.8" *) MISTRAL_FF zero_ff (
 .CLK(clk), .DATAIN(1'b0), .Q(q_zero), .ACLR(1'b1), .ENA(1'b1),
 .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
endmodule
''')
    declarations = [('BEL', 'MISTRAL_COMB.24.1.0'), ('RECT', '24 1 25 1'),
                    ('RECT', 's1 33 1 33 1'), ('RECT', 's2 34 1 34 1'),
                    ('RECT', 's3 35 1 35 1'), ('GROUP', 'inner s1 s2'),
                    ('GROUP', 'outer inner s3')]
    pins = ('set_location_assignment PIN_V11 -to clk\n'
            'set_location_assignment PIN_Y13 -to a\n'
            'set_location_assignment PIN_W15 -to q_static\n'
            'set_location_assignment PIN_AA24 -to q_cart\n'
            'set_location_assignment PIN_E11 -to q_zero\n')
    text = pins + ''.join(f'set_global_assignment -name FES_RESERVED_{kind if kind != "GROUP" else "RECT_GROUP"} "{spec}"\n'
                          for kind, spec in declarations)
    qsf = out / 'pins.qsf'
    qsf.write_text(text)
    def run(command, label, fail=None):
        log = out / (label + '.log')
        with log.open('w') as stream:
            result = subprocess.run([str(x) for x in command], stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=600)
        if fail:
            assert result.returncode != 0 and fail in log.read_text(), log
        else:
            assert result.returncode == 0, log
    run([args.yosys.resolve(), '-p', f'read_verilog {source}; synth_intel_alm -nolutram -nodsp -top top; '
         f'write_json {out / "synth.json"}'], 'synth')
    base = [args.nextpnr.resolve(), '--device', '5CSEBA6U23I7', '--freq', '50',
            '--timing-allow-fail', '--compress-rbf']
    run(base + ['--json', out / 'synth.json', '--qsf', qsf, '--no-route',
                '--write', out / 'placed.json'], 'place')
    def load(name):
        return json.loads((out / name).read_text())['modules']['top']
    key = 'mistral/fes_reservations_v1'
    expected = load('placed.json')['settings'][key]
    assert json.loads(expected) == [list(x) for x in declarations]
    resume = base + ['--json', out / 'placed.json', '--no-pack', '--no-place']
    run(resume + ['--no-route', '--write', out / 'copied.json'], 'copy')
    assert load('copied.json')['settings'][key] == expected
    run(resume + ['--qsf', qsf, '--no-route', '--write', out / 'qsf-copy.json'], 'same-qsf')
    assert load('qsf-copy.json')['settings'][key] == expected
    run(resume + ['--write', out / 'routed.json', '--rbf', out / 'original.rbf'], 'route')
    routed = load('routed.json')
    assert routed['settings'][key] == expected
    assert 'static_ff$ROUTETHRU' not in routed['cells']
    assert 'zero_ff$ROUTETHRU' not in routed['cells']
    helper = routed['cells']['cart_ff$ROUTETHRU']
    assert helper['attributes']['NEXTPNR_BEL'] in ('MISTRAL_COMB.25.1.0', 'MISTRAL_MCOMB.25.1.0')
    assert helper['attributes']['FES_SLOT'] == 'cart'
    def aliases(module, bit):
        return sorted((name, index) for name, net in module['netnames'].items()
                      for index, value in enumerate(net['bits']) if value == bit)
    placed = load('placed.json')
    assert aliases(placed, placed['cells']['static_ff']['connections']['DATAIN'][0]) == aliases(
        routed, routed['cells']['static_ff']['connections']['DATAIN'][0])
    run(base + ['--json', out / 'routed.json', '--no-pack', '--no-place', '--no-route',
                '--write', out / 'reloaded.json'], 'replay')
    assert load('reloaded.json')['settings'][key] == expected
    bel_qsf=out/'bel-only.qsf'
    bel_qsf.write_text(pins + 'set_global_assignment -name FES_RESERVED_BEL \"MISTRAL_COMB.24.1.0\"\n')
    bel_input=json.loads((out/'synth.json').read_text())
    del bel_input['modules']['top']['cells']['cart_ff']['attributes']['FES_SLOT']
    (out/'bel-synth.json').write_text(json.dumps(bel_input))
    run(base + ['--json',out/'bel-synth.json','--qsf',bel_qsf,'--no-route','--write',out/'bel-placed.json'],'bel-place')
    run(base + ['--json',out/'bel-placed.json','--no-pack','--no-place','--write',out/'bel-routed.json','--rbf',out/'bel-original.rbf'],'bel-route')
    assert 'static_ff$ROUTETHRU' not in load('bel-routed.json')['cells']
    assert 'cart_ff$ROUTETHRU' in load('bel-routed.json')['cells']
    run(base + ['--json',out/'bel-routed.json','--no-pack','--no-place','--no-route',
                '--rbf',out/'bel-replayed.rbf'],'bel-replay')
    assert (out/'bel-original.rbf').read_bytes() == (out/'bel-replayed.rbf').read_bytes()
    for label, content in [('changed-qsf', text.replace('outer inner s3', 'outer s3 inner')),
                           ('partial-qsf', pins + 'set_global_assignment -name FES_RESERVED_RECT "24 1 25 1"\n')]:
        path = out / (label + '.qsf')
        path.write_text(content)
        run(resume + ['--qsf', path, '--no-route'], label, 'QSF FES reservations differ')
    for label, value in [('bad-json', 'no'), ('bad-nul', json.dumps([['RECT', '24 1 25 1\0hidden']])), ('bad-kind', '[["BOGUS","x"]]'),
                         ('bad-shape', '[["RECT"]]'), ('bad-region', '[["GROUP","g missing"]]')]:
        design = copy.deepcopy(json.loads((out / 'placed.json').read_text()))
        design['modules']['top']['settings'][key] = value
        path = out / (label + '.json')
        path.write_text(json.dumps(design))
        run(base + ['--json', path, '--no-pack', '--no-place', '--no-route'], label,
            'FES_RESERVED_RECT_GROUP' if label == 'bad-region' else 'Invalid FES reservation snapshot')
    design = json.loads((out / 'placed.json').read_text())
    design['modules']['top']['settings']['mistral/fes_reservations_v2'] = expected
    path = out / 'bad-version.json'
    path.write_text(json.dumps(design))
    run(base + ['--json', path, '--no-pack', '--no-place', '--no-route'], 'bad-version',
        'Unsupported FES reservation snapshot version')
    print('FES reservation checkpoint and route preparation passed')


if __name__ == '__main__':
    main()
