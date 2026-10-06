#!/usr/bin/env python3
"""Inventory SDRAM constraint declarations; passing is not board timing signoff.

Only structurally constant combinational outputs are exempted. Clock waveform,
OE turnaround, board bounds, model qualification and SDC cuts need separate
checks even if every active direction has an input/output delay declaration.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


def inventory(module):
    cells = module['cells']
    drivers = {}
    pads = {}
    for name, cell in cells.items():
        for pin, bits in cell['connections'].items():
            if pin == 'PAD':
                for bit in bits:
                    if bit in pads:
                        raise ValueError(f'Ambiguous pad for bit {bit}')
                    pads[bit] = (name, cell)
            if cell['port_directions'][pin] == 'output':
                for bit in bits:
                    drivers.setdefault(bit, []).append((name, cell))

    def constant(bit):
        if bit in ('0', '1'):
            return True
        sources = drivers.get(bit, [])
        return (len(sources) == 1 and sources[0][1]['type'] == 'MISTRAL_CONST' and
                sources[0][1].get('parameters', {}).get('LUT') in ('0'*32, '1'*32))

    saved = json.loads(module.get('settings', {}).get('timing/io_delays', '[]'))
    constraints = {}
    for row in saved:
        key = (row['port'], row['input'])
        if not isinstance(row['input'], bool) or key in constraints:
            raise ValueError('Ambiguous IO delay direction')
        if not all(isinstance(row[x], (int, float)) and not isinstance(row[x], bool) and
                   math.isfinite(row[x]) for x in ('min', 'max')) or row['min'] > row['max']:
            raise ValueError('Invalid IO delay bounds')
        constraints[key] = row

    rows = []
    for name, port in sorted(module['ports'].items()):
        if not name.startswith('SDRAM_'):
            continue
        if port.get('upto'):
            raise ValueError('Ascending buses require explicit bit-name normalization')
        for index, bit in enumerate(port['bits']):
            offset = port.get('offset', 0)
            literal = name if len(port['bits']) == 1 and offset == 0 else f'{name}[{index+offset}]'
            directions = {'input': [True], 'output': [False], 'inout': [True, False]}[port['direction']]
            pad_name, cell = pads.get(bit, (None, {}))
            # A registered constant output is not silently exempted: its reset
            # and clock behavior have not been proven by this structural check.
            static = (port['direction'] == 'output' and cell.get('type') == 'MISTRAL_OB' and
                      len(cell.get('connections', {}).get('I', [])) == 1 and
                      constant(cell['connections']['I'][0]))
            for input_direction in directions:
                constraint = constraints.get((literal, input_direction))
                rows.append(dict(port=literal, direction='input' if input_direction else 'output',
                                 pad_cell=pad_name, pad_type=cell.get('type'),
                                 timing_profile=cell.get('attributes', {}).get('NEXTPNR_GPIO_TIMING_PROFILE'),
                                 exempt_structural_constant=static, constraint=constraint,
                                 declaration_present=static or constraint is not None))
    if not rows:
        raise ValueError('No SDRAM ports found')
    active = [row for row in rows if not row['exempt_structural_constant']]
    missing = [dict(port=row['port'], direction=row['direction']) for row in active
               if not row['declaration_present']]
    return dict(classification='constraint declaration inventory, not timing or board signoff',
                active_directions=len(active), declared_directions=len(active)-len(missing),
                all_active_directions_declared=not missing, missing=missing, ports=rows,
                remaining_checks=['actual timed paths and clock exceptions',
                                  'pad model and electrical/load qualification',
                                  'chip/board setup and hold bounds',
                                  'forwarded clock period and pulse widths',
                                  'DQ output-enable bus turnaround',
                                  'controller capture/consumption cycle', 'hardware acceptance'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inventory(json.loads(args.checkpoint.read_text())['modules']['top'])
    result['checkpoint_sha256'] = hashlib.sha256(args.checkpoint.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({key: result[key] for key in
                      ('active_directions', 'declared_directions', 'all_active_directions_declared', 'missing')},
                     indent=2))
    raise SystemExit(0 if result['all_active_directions_declared'] else 1)


if __name__ == '__main__':
    main()
