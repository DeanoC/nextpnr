#!/usr/bin/env python3
"""Route a RAM input mapped to one or two physical pins, with and without a driver."""
import argparse
import json
from pathlib import Path
import subprocess


def fixture(driven, multiple):
    # A packed 10-bit TDP RAM duplicates each data input onto the two 10-bit
    # halves of the physical port. Keep that real mapping in this small placed
    # checkpoint: two logical users with one pin each do not trigger the bug.
    pins = ['DATABIN[7]', 'DATABIN[17]'] if multiple else ['DATABIN[7]']

    def cell(kind, bel, parameters, mapping, directions, connections):
        snapshot = {'count': len(mapping), 'pins': mapping}
        return {'type': kind, 'parameters': parameters,
                'attributes': {'NEXTPNR_BEL': bel,
                               'FES_PINMAP_V1': json.dumps(snapshot).encode().hex()},
                'port_directions': directions, 'connections': connections}

    ram = cell('MISTRAL_M10K', 'MISTRAL_M10K.41.20.0',
               {'CFG_TDP': '1', 'CFG_DBITS': '1010', 'CFG_ABITS': '1010'},
               {'B1DATA[7]': [0] + pins}, {'B1DATA[7]': 'input'}, {'B1DATA[7]': [2]})
    module = {'attributes': {'top': '1', 'step': 'place'}, 'ports': {},
              'cells': {'ram': ram}, 'settings': {'timing_driven': '1'},
              'netnames': {'signal': {'bits': [2], 'attributes': {}}}}
    if driven:
        module['cells']['driver'] = cell(
            'MISTRAL_ALUT2', 'MISTRAL_COMB.24.1.0', {'LUT': '1010'},
            {'A': [1, 'F0'], 'B': [1, 'E0'], 'Q': [0, 'COMBOUT']},
            {'A': 'input', 'B': 'input', 'Q': 'output'}, {'A': [3], 'B': [4], 'Q': [2]})
        # These inputs are already folded to zero in the physical snapshot.
        # Numeric, driverless nets avoid introducing unplaced constant cells.
        module['netnames'].update({name: {'bits': [bit], 'attributes': {}}
                                   for name, bit in [('folded_a', 3), ('folded_b', 4)]})
    return {'modules': {'top': module}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nextpnr', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for driven in (False, True):
        for multiple in (False, True):
            name = ('driven' if driven else 'undriven') + ('-two' if multiple else '-one')
            placed, routed, log = (out / (name + suffix) for suffix in ('.json', '-routed.json', '.log'))
            placed.write_text(json.dumps(fixture(driven, multiple)))
            with log.open('w') as stream:
                result = subprocess.run([
                    str(args.nextpnr.resolve()), '--json', str(placed),
                    '--device', '5CSEBA6U23I7', '--router', 'router2',
                    '--no-pack', '--no-place', '--timing-allow-fail', '--write', str(routed)],
                    stdout=stream, stderr=subprocess.STDOUT, timeout=120)
            if result.returncode != 0:
                raise RuntimeError(f'{name} exited {result.returncode}: see {log}')
            module = json.loads(routed.read_text())['modules']['top']
            route = module['netnames']['signal']['attributes']['ROUTING'].strip()
            if driven:
                # Assert both physical sinks remain routed from the LUT driver.
                wires = set(route.split(';')[::3])
                assert 'WIRE.24.1.COMBOUT[0]' in wires, (name, route)
                assert 'GOUT.41.20.75' in wires, (name, route)
                if multiple:
                    assert 'GOUT.41.20.73' in wires, (name, route)
            else:
                assert not route, (name, route)
                assert set(module['cells']) == {'ram'}, module['cells'].keys()
    print('router2 undriven/multiple-physical-pin regression passed')


if __name__ == '__main__':
    main()
