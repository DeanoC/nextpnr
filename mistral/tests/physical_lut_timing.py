#!/usr/bin/env python3
"""Identical routed LUT hardware must retain identical setup/hold timing."""
import argparse
import copy
import json
from pathlib import Path
import subprocess


def permute_inputs(cell, order):
    original = copy.deepcopy(cell)
    keys = 'ABCDEF'[:len(order)]
    mask = int(original['parameters']['LUT'], 2)
    new_mask = 0
    for value in range(1 << len(order)):
        old_value = sum(((value >> i) & 1) << order[i] for i in range(len(order)))
        new_mask |= ((mask >> old_value) & 1) << value
    cell['parameters']['LUT'] = format(new_mask, f'0{1 << len(order)}b')
    pins = json.loads(bytes.fromhex(original['attributes']['FES_PINMAP_V1']))
    old_pins = copy.deepcopy(pins['pins'])
    for i, key in enumerate(keys):
        source = keys[order[i]]
        cell['connections'][key] = original['connections'][source]
        pins['pins'][key] = old_pins[source]
    cell['attributes']['FES_PINMAP_V1'] = json.dumps(pins).encode().hex()
    before = {tuple(old_pins[k][1:]): (original['connections'][k], old_pins[k][0]) for k in keys}
    after = {tuple(pins['pins'][k][1:]): (cell['connections'][k], pins['pins'][k][0]) for k in keys}
    assert before == after


def endpoint_timings(report):
    return {(net['driver'], net['port'], net['event'], endpoint['cell'], endpoint['port'], endpoint['event']):
            {key: endpoint.get(key) for key in ('setup_checked', 'hold_checked', 'setup_slack_ns',
                                               'hold_slack_ns', 'delay')}
            for net in report['detailed_net_timings'] for endpoint in net['endpoints']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('yosys', 'nextpnr', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = out / 'top.v'
    fixture.write_text('''module top(input clk, a, output reg [1:0] q);
reg [5:0] source;
wire inverted, five, six;
MISTRAL_NOT invert (.A(source[1]), .Q(inverted));
MISTRAL_ALUT5 #(.LUT(32'h69ca96a5)) l5 (
    .A(source[0]), .B(inverted), .C(source[2]), .D(source[3]), .E(source[4]), .Q(five));
MISTRAL_ALUT6 #(.LUT(64'h69969669a5965ac3)) l6 (
    .A(source[0]), .B(source[1]), .C(source[2]), .D(source[3]), .E(source[4]), .F(source[5]), .Q(six));
always @(posedge clk) begin
    source <= {source[4:0], a};
    q <= {six, five};
end
endmodule
''')
    qsf = out / 'pins.qsf'
    qsf.write_text('set_location_assignment PIN_V11 -to clk\n'
                   'set_location_assignment PIN_Y13 -to a\n'
                   'set_location_assignment PIN_W15 -to q[0]\n'
                   'set_location_assignment PIN_AA24 -to q[1]\n')
    lib = args.yosys.resolve().parent.parent / 'share/yosys/intel_alm/common/alm_sim.v'

    def run(command, label):
        with (out / (label + '.log')).open('w') as log:
            result = subprocess.run([str(x) for x in command], stdout=log,
                                    stderr=subprocess.STDOUT, timeout=1200)
        if result.returncode:
            raise RuntimeError(f'{label} exited {result.returncode}: see {out / (label + ".log")}')

    run([args.yosys.resolve(), '-p', f'read_verilog -lib {lib}; read_verilog {fixture}; '
         f'synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {out / "synth.json"}'], 'synth')
    base = [args.nextpnr.resolve(), '--device', '5CSEBA6U23I7', '--freq', '50',
            '--timing-allow-fail', '--compress-rbf', '--detailed-timing-report']
    run(base + ['--json', out / 'synth.json', '--qsf', qsf, '--write', out / 'routed.json',
                '--rbf', out / 'original.rbf', '--report', out / 'original-timing.json'], 'route')
    original = json.loads((out / 'routed.json').read_text())
    cells = original['modules']['top']['cells']
    assert cells['l5']['type'] == 'MISTRAL_ALUT5' and cells['l6']['type'] == 'MISTRAL_ALUT6'
    pins = json.loads(bytes.fromhex(cells['l5']['attributes']['FES_PINMAP_V1']))['pins']
    assert pins['B'][0] == 3, 'fixture must include the packer-folded input inversion'
    expected = endpoint_timings(json.loads((out / 'original-timing.json').read_text()))
    assert expected
    # Exercise each physical input under a different logical name, including
    # the two half-specific L6 selectors and the folded L5 inversion.
    for shift in range(1, 6):
        design = copy.deepcopy(original)
        for name, width in [('l5', 5), ('l6', 6)]:
            permute_inputs(design['modules']['top']['cells'][name],
                           [(i + shift) % width for i in range(width)])
        checkpoint = out / f'permuted-{shift}.json'
        checkpoint.write_text(json.dumps(design))
        rbf, report = out / f'permuted-{shift}.rbf', out / f'permuted-{shift}-timing.json'
        run(base + ['--json', checkpoint, '--no-pack', '--no-place', '--no-route',
                    '--rbf', rbf, '--report', report], f'replay-{shift}')
        assert rbf.read_bytes() == (out / 'original.rbf').read_bytes(), shift
        actual = endpoint_timings(json.loads(report.read_text()))
        assert actual == expected, (shift, [(key, expected.get(key), value)
                                          for key, value in actual.items() if expected.get(key) != value])
    print('PASS: five L5/L6 logical permutations preserve bitstream and every endpoint timing')


if __name__ == '__main__':
    main()
