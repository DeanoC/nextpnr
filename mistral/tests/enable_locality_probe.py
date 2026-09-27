#!/usr/bin/env python3
"""Check the isolated post-placement enable clone, before any route changes."""
import argparse
import copy
import json

DRIVER = 'hps_ddr.port1.slot_free_MISTRAL_ALUT3_B'
CLONE = DRIVER + '$LOCALITY_PROBE'
LABS = {(34, 22), (34, 25), (35, 25)}


def canonical(module):
    module = copy.deepcopy(module)
    aliases = {}
    for name, net in module['netnames'].items():
        for index, bit in enumerate(net['bits']):
            if isinstance(bit, int):
                aliases.setdefault(bit, []).append((name, index))
    def convert(bits):
        return [min(aliases[bit]) if isinstance(bit, int) else bit for bit in bits]
    for cell in module['cells'].values():
        cell['connections'] = {port: convert(bits) for port, bits in cell['connections'].items()}
    for net in module['netnames'].values():
        net['bits'] = convert(net['bits'])
    for port in module['ports'].values():
        port['bits'] = convert(port['bits'])
    return module


def read_pin_states(path):
    states = {}
    with open(path) as handle:
        assert next(handle).strip() == 'cell\tport\tstate'
        for line in handle:
            cell, port, state = line.rstrip('\n').split('\t')
            assert (cell, port) not in states
            states[(cell, port)] = int(state)
    return states


def check_pin_states(before, after, original_cells, clone_cell):
    assert before, 'missing original pin states'
    for name, cell in original_cells.items():
        for port in cell['port_directions']:
            # nextpnr JSON groups indexed cell ports into a bus; sidecars retain pin names.
            pins = [port] if (name, port) in before else [f'{port}[{i}]' for i in range(len(cell['connections'][port]))]
            assert all((name, pin) in before for pin in pins), f'missing original pin evidence: {name}.{port}'
    for key, state in before.items():
        assert after.get(key) == state, f'original folded pin state changed: {key}'
    expected_clone = {(CLONE, port): state for (name, port), state in before.items() if name == DRIVER}
    assert expected_clone and all((CLONE, port) in expected_clone for port in clone_cell['port_directions'])
    assert set(after) - set(before) == set(expected_clone), 'unexpected added/missing pin state records'
    for key, state in expected_clone.items():
        assert after[key] == state, f'clone folded pin state differs: {key}'
    return len(before)


def check(before, after, before_pins, after_pins):
    b, a = canonical(before['modules']['top']), canonical(after['modules']['top'])
    bc, ac = b.pop('cells'), a.pop('cells')
    assert set(ac) - set(bc) == {CLONE}, 'expected exactly the diagnostic clone cell'
    assert set(bc) - set(ac) == set(), 'original cells removed'
    original, clone = bc[DRIVER], ac[CLONE]
    assert original['type'] == clone['type'] == 'MISTRAL_ALUT3'
    assert original['parameters'] == clone['parameters']
    assert original['port_directions'] == clone['port_directions']
    for port in ('A', 'B', 'C'):
        assert original['connections'][port] == clone['connections'][port]
    ignore = {'NEXTPNR_BEL', 'BEL_STRENGTH'}
    assert {k:v for k,v in original['attributes'].items() if k not in ignore} == {
        k:v for k,v in clone['attributes'].items() if k not in ignore}, 'clone attributes/polarity changed'
    old_net = original['connections']['Q']
    new_net = clone['connections']['Q']
    assert old_net != new_net
    moved = []
    seen = set()
    for name, cell in bc.items():
        wanted = copy.deepcopy(cell)
        loc = tuple(map(int, cell['attributes']['NEXTPNR_BEL'].split('.')[1:3]))
        if cell['connections'].get('ENA') == old_net and loc in LABS:
            assert cell['type'] == 'MISTRAL_FF'
            wanted['connections']['ENA'] = new_net
            moved.append(name)
            seen.add(loc)
        assert wanted == ac[name], f'unexpected cell change: {name}'
    assert seen == LABS and len(moved) == 5, 'expected every specified LAB to contribute sinks'
    pin_count = check_pin_states(before_pins, after_pins, bc, clone)
    # Netnames include one new net; every original net and all design metadata stay unchanged.
    old_names, new_names = b.pop('netnames'), a.pop('netnames')
    assert set(new_names) - set(old_names) == {CLONE + '$Q'}
    for name, net in old_names.items():
        assert new_names[name] == net, f'original net metadata changed: {name}'
    assert a == b, 'ports, initialization metadata or design settings changed'
    result = {'original_driver': DRIVER, 'clone': CLONE,
              'original_bel': original['attributes']['NEXTPNR_BEL'],
              'clone_bel': clone['attributes']['NEXTPNR_BEL'], 'moved_sinks': sorted(moved),
              'original_placements_unchanged': len(bc), 'original_pin_states_unchanged': pin_count, 'truth_table_identical': original['parameters']}
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before')
    parser.add_argument('after')
    args = parser.parse_args()
    check(json.load(open(args.before)), json.load(open(args.after)),
          read_pin_states(args.before + '.pins.tsv'), read_pin_states(args.after + '.pins.tsv'))


if __name__ == '__main__':
    main()
