#!/usr/bin/env python3
"""Check placed-checkpoint preservation of packer-folded input states."""
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
    fixture = out / 'top.v'
    fixture.write_text('''module top(input clk, a, b, output [1:0] q);
wire inverted_a, sum, carry, chain_carry;
reg [1:0] value;
MISTRAL_NOT invert_a (.A(a), .Q(inverted_a));
MISTRAL_ALUT_ARITH #(.LUT0(16'hAAAA), .LUT1(16'hCCCC)) arithmetic (
    .A(inverted_a), .B(b), .C(1'b0), .D0(1'b1), .D1(1'b1), .CI(1'b0),
    .SO(sum), .CO(chain_carry));
MISTRAL_ALUT_ARITH #(.LUT0(16'h0000), .LUT1(16'hFFFF)) carry_end (
    .A(1'b0), .B(1'b0), .C(1'b0), .D0(1'b1), .D1(1'b1),
    .CI(chain_carry), .SO(carry));
always @(posedge clk) value <= {carry, sum};
assign q = value;
endmodule
''')
    qsf = out / 'pins.qsf'
    qsf.write_text('set_location_assignment PIN_V11 -to clk\n'
                   'set_location_assignment PIN_Y13 -to a\n'
                   'set_location_assignment PIN_E11 -to b\n'
                   'set_location_assignment PIN_W15 -to q[0]\n'
                   'set_location_assignment PIN_AA24 -to q[1]\n')
    lib = args.yosys.resolve().parent.parent / 'share/yosys/intel_alm/common/alm_sim.v'

    def run(command, label, accepted=(0,)):
        with (out / (label + '.log')).open('w') as log:
            result = subprocess.run([str(x) for x in command], stdout=log,
                                    stderr=subprocess.STDOUT, timeout=1200)
        if result.returncode not in accepted:
            raise RuntimeError(f'{label} exited {result.returncode}: see {out / (label + ".log")}')

    run([args.yosys.resolve(), '-p', f'read_verilog -lib {lib}; read_verilog {fixture}; '
         f'synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {out / "synth.json"}'], 'synth')
    base = [args.nextpnr.resolve(), '--device', '5CSEBA6U23I7', '--freq', '50',
            '--timing-allow-fail', '--compress-rbf']
    run(base + ['--json', out / 'synth.json', '--qsf', qsf, '--no-route',
                '--write', out / 'placed.json'], 'place')
    placed = json.loads((out / 'placed.json').read_text())

    def pin_states(design):
        return {n: json.loads(bytes.fromhex(c['attributes']['FES_PINMAP_V1']))['pins']
                for n, c in design['modules']['top']['cells'].items()
                if 'FES_PINMAP_V1' in c['attributes']}

    expected = pin_states(placed)
    assert expected and expected['arithmetic']['A'][0] == 3, expected.get('arithmetic')
    assert any(data[0] == 2 for pins in expected.values() for data in pins.values())
    run(base + ['--json', out / 'placed.json', '--no-pack', '--no-place',
                '--write', out / 'routed.json', '--rbf', out / 'original.rbf'], 'resume')
    routed = json.loads((out / 'routed.json').read_text())
    actual = pin_states(routed)
    for n, pins in expected.items():
        for port, data in pins.items():
            assert actual[n][port][0] == data[0], (n, port, data, actual[n][port])
    run(base + ['--json', out / 'routed.json', '--no-pack', '--no-place', '--no-route',
                '--rbf', out / 'replayed.rbf'], 'replay')
    assert (out / 'original.rbf').read_bytes() == (out / 'replayed.rbf').read_bytes()

    def corrupt(label, edit, expected_error):
        bad = copy.deepcopy(placed)
        cell = bad['modules']['top']['cells']['arithmetic']
        payload = json.loads(bytes.fromhex(cell['attributes']['FES_PINMAP_V1']))
        edit(cell, payload)
        if label != 'unknown-version':
            cell['attributes']['FES_PINMAP_V1'] = json.dumps(payload).encode().hex()
        f = out / (label + '.json')
        f.write_text(json.dumps(bad))
        run(base + ['--json', f, '--no-pack', '--no-place', '--no-route'], label, accepted=(1, 125))
        assert expected_error in (out / (label + '.log')).read_text()

    corrupt('invalid-state', lambda c, p: p['pins']['A'].__setitem__(0, 4), 'Invalid frozen pin state')
    corrupt('invalid-physical-pin', lambda c, p: p['pins']['A'].append('NOT_A_BEL_PIN'),
            'Invalid frozen physical pin')

    def incomplete(c, p):
        del p['pins']['A']
        p['count'] = len(p['pins'])

    corrupt('incomplete', incomplete, 'Incomplete frozen pin map')

    def unknown_version(c, p):
        c['attributes']['FES_PINMAP_V2'] = c['attributes'].pop('FES_PINMAP_V1')

    corrupt('unknown-version', unknown_version, 'Unsupported frozen pin-map version')
    print('PASS: placed folded inversions/constants survive routing; identical frozen replay; four malformed snapshots rejected')


if __name__ == '__main__':
    main()
