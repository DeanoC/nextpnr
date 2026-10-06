#!/usr/bin/env python3
"""Physical SDR output packing; deliberately no external timing acceptance."""
import argparse
import copy
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for key in ('yosys', 'nextpnr', 'mistral-cv', 'output'):
    p.add_argument('--' + key, required=True, type=Path)
a = p.parse_args()
f = Path(__file__).resolve().parent
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)

def run(cmd, log, error=None):
    r = subprocess.run([str(x) for x in cmd], capture_output=True, text=True, timeout=180)
    text = r.stdout + r.stderr
    log.write_text(text)
    if error:
        assert r.returncode != 0 and error in text, text[-3000:]
    else:
        assert r.returncode == 0, text[-3000:]
    return text

run([a.yosys, '-p', f'read_verilog {f / "top.v"}; synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {o / "input.json"}'], o / 'synth.log')
original = json.loads((o / 'input.json').read_text())
base = [a.nextpnr, '--device', '5CSEBA6U23I7', '--freq', '50']
settings = {'OEREG_HR_CLK_EN.9': '1', 'OUTREG_OUTPUT_SEL.9': 'SEL_SDR',
            'RBOE_LVL_FR_CLK_EN.9': '1'}
for name in ('direct', 'zero', 'one', 'delayed', 'disabled', 'inverted', 'sclr', 'inverted-sclr'):
    case = o / name
    case.mkdir(exist_ok=True)
    j = copy.deepcopy(original)
    ff = next(c for c in j['modules']['top']['cells'].values() if c['type'] == 'MISTRAL_FF')
    if name in ('zero', 'one'):
        ff['connections']['DATAIN'] = ['1' if name == 'one' else '0']
    if name in ('inverted', 'inverted-sclr'):
        output = ff['connections']['Q']
        ff['connections']['Q'] = [9]
        j['modules']['top']['cells']['output_inverter'] = {
            'hide_name': 0, 'type': 'MISTRAL_NOT', 'parameters': {}, 'attributes': {},
            'port_directions': {'A': 'input', 'Q': 'output'}, 'connections': {'A': [9], 'Q': output}}
    if name in ('sclr', 'inverted-sclr'):
        ff['connections']['SCLR'] = ff['connections']['DATAIN']
    (case / 'input.json').write_text(json.dumps(j))
    qsf = (f / 'pins.qsf').read_text()
    if name == 'disabled':
        qsf = qsf.replace('REGISTER ON', 'REGISTER OFF')
    if name == 'delayed':
        qsf += 'set_instance_assignment -name D5_DELAY 31 -to SDR_OUT\n'
    (case / 'pins.qsf').write_text(qsf)
    log = run(base + ['--json', case / 'input.json', '--qsf', case / 'pins.qsf', '--sdc', f / 'clocks.sdc',
                     '--write', case / 'routed.json', '--report', case / 'timing.json', '--compress-rbf', '--rbf', case / 'top.rbf'], case / 'route.log')
    cells = json.loads((case / 'routed.json').read_text())['modules']['top']['cells']
    sdr = [c for c in cells.values() if c['type'] == 'MISTRAL_SDROUT']
    assert len(sdr) == (0 if name == 'disabled' else 1)
    if name == 'disabled':
        assert any(c['type'] == 'MISTRAL_FF' for c in cells.values())
        continue
    assert not any(c['type'] == 'MISTRAL_FF' for c in cells.values())
    assert sdr[0]['connections']['CLK'] and sdr[0]['connections']['I']
    assert 'clock-to-pad timing is uncharacterized' in log
    run([a.mistral_cv, 'decomp', '5CSEBA6U23I7', case / 'top.rbf', case / 'top.bt'], case / 'decode.log')
    bt = (case / 'top.bt').read_text()
    actual = {}
    for line in bt.splitlines():
        if line.startswith('s DQS16.089.008:'):
            key, value = line.split()[1:3]
            key = key.split(':')[1]
            if key.endswith('.9'): actual[key] = value
    expected = dict(settings)
    if name == 'delayed':
        expected['RB_T9_SEL_OREG_DFF_DELAY.9'] = '1f'
    if name in ('inverted', 'inverted-sclr'):
        expected['OUTREG_POWER_UP_STATE.9'] = '1'
        assert any(c['type'] == 'MISTRAL_NOT' for c in cells.values())
        assert int(sdr[0]['parameters']['IOREG_OUT_POWER_UP'], 2) == 1
    if name in ('sclr', 'inverted-sclr'):
        selects = [c for c in cells.values() if c['type'] == 'MISTRAL_ALUT2']
        assert len(selects) == 1 and int(selects[0]['parameters']['LUT'], 2) == 2
    assert actual == expected, actual
    run(base + ['--json', case / 'routed.json', '--no-pack', '--no-place', '--no-route',
                '--compress-rbf', '--rbf', case / 'replayed.rbf'], case / 'replay.log')
    assert (case / 'top.rbf').read_bytes() == (case / 'replayed.rbf').read_bytes()
    # The pad clock DCMUX keeps its default TCLK input, which the decoder
    # does not print as a route; check the routed clock reaches it.
    nets = json.loads((case / 'routed.json').read_text())['modules']['top']['netnames']
    clock_routing = ' '.join(n.get('attributes', {}).get('ROUTING', '') for n in nets.values())
    assert 'DCMUX.89.8.' in clock_routing and ':DATAOUT.0 ; W15' in bt
    print('PASS', name, 'packed GPIO, clock/data routes and oracle settings')

for name, reason in [('reset', 'asynchronous clear is held active'),
                     ('load', 'synchronous clear and load have no I/O register equivalent'),
                     ('clock', 'clock must be driven'),
                     ('parameter', 'unsupported register parameters'),
                     ('inverted-clock', 'noninverted clock source'),
                     ('q-fanout', 'no other Q consumers'),
                     ('inverted-aclr', 'unsupported preset'),
                     ('inverter-fanout', 'no other Q consumers')]:
    j = copy.deepcopy(original)
    ff = next(c for c in j['modules']['top']['cells'].values() if c['type'] == 'MISTRAL_FF')
    if name == 'reset': ff['connections']['ACLR'] = ['0']
    if name == 'load': ff['connections']['SLOAD'] = ['1']
    if name == 'clock': ff['connections']['CLK'] = ['0']
    if name == 'parameter': ff['parameters']['UNSUPPORTED'] = '1'
    if name == 'inverted-clock':
        ff['connections']['CLK'] = [9]
        j['modules']['top']['cells']['inverted_clock'] = {
            'hide_name': 0, 'type': 'MISTRAL_NOT', 'parameters': {}, 'attributes': {},
            'port_directions': {'A': 'input', 'Q': 'output'}, 'connections': {'A': [6], 'Q': [9]}}
    if name == 'q-fanout':
        j['modules']['top']['cells']['q_fanout'] = {
            'hide_name': 0, 'type': 'MISTRAL_BUF', 'parameters': {}, 'attributes': {},
            'port_directions': {'A': 'input', 'Q': 'output'}, 'connections': {'A': [7], 'Q': [9]}}
    if name in ('inverted-aclr', 'inverter-fanout'):
        output = ff['connections']['Q']
        ff['connections']['Q'] = [9]
        j['modules']['top']['cells']['output_inverter'] = {
            'hide_name': 0, 'type': 'MISTRAL_NOT', 'parameters': {}, 'attributes': {},
            'port_directions': {'A': 'input', 'Q': 'output'}, 'connections': {'A': [9], 'Q': output}}
        if name == 'inverted-aclr':
            ff['connections']['ACLR'] = ff['connections']['DATAIN']
        else:
            j['modules']['top']['cells']['extra_consumer'] = {
                'hide_name': 0, 'type': 'MISTRAL_BUF', 'parameters': {}, 'attributes': {},
                'port_directions': {'A': 'input', 'Q': 'output'}, 'connections': {'A': output, 'Q': [10]}}
    inp = o / (name + '.json')
    inp.write_text(json.dumps(j))
    run(base + ['--json', inp, '--qsf', f / 'pins.qsf'], o / (name + '.log'), reason)
sdc = o / 'set_output_delay.sdc'
sdc.write_text((f / 'clocks.sdc').read_text() +
               'set_output_delay -clock FPGA_CLK1_50 2.0 [get_ports SDR_OUT]\n')
run(base + ['--json', o / 'input.json', '--qsf', f / 'pins.qsf', '--sdc', sdc],
    o / 'set_output_delay.log', 'no supported unregistered IO timing boundary')
print('PASS disabled packing, eight unsupported register requests and registered external timing rejection')
