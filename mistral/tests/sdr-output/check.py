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
            'RB_T9_SEL_EREG_CFF_DELAY.9': '1f', 'RB_T9_SEL_OREG_DFF_DELAY.9': '1f',
            'RBOE_LVL_FR_CLK_EN.9': '1'}
for name in ('direct', 'zero', 'one', 'disabled'):
    case = o / name
    case.mkdir(exist_ok=True)
    j = copy.deepcopy(original)
    ff = next(c for c in j['modules']['top']['cells'].values() if c['type'] == 'MISTRAL_FF')
    if name in ('zero', 'one'):
        ff['connections']['DATAIN'] = ['1' if name == 'one' else '0']
    (case / 'input.json').write_text(json.dumps(j))
    qsf = (f / 'pins.qsf').read_text()
    if name == 'disabled':
        qsf = qsf.replace('REGISTER ON', 'REGISTER OFF')
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
    assert 'setup/hold and clock-to-pad timing are uncharacterized' in log
    run([a.mistral_cv, 'decomp', '5CSEBA6U23I7', case / 'top.rbf', case / 'top.bt'], case / 'decode.log')
    bt = (case / 'top.bt').read_text()
    actual = {}
    for line in bt.splitlines():
        if line.startswith('s DQS16.089.008:'):
            key, value = line.split()[1:3]
            key = key.split(':')[1]
            if key.endswith('.9'): actual[key] = value
    assert actual == settings, actual
    assert ':CLKOUT.0 ; W15' in bt and ':DATAOUT.0 ; W15' in bt
    print('PASS', name, 'packed GPIO, clock/data routes and oracle settings')

for name, reason in [('enable', 'constant ENA/ACLR'), ('reset', 'constant ENA/ACLR'),
                     ('load', 'constant ENA/ACLR'), ('clock', 'clock must be driven'),
                     ('parameter', 'unsupported register parameters'),
                     ('inverted-clock', 'noninverted clock source'),
                     ('q-fanout', 'no other Q consumers')]:
    j = copy.deepcopy(original)
    ff = next(c for c in j['modules']['top']['cells'].values() if c['type'] == 'MISTRAL_FF')
    if name == 'enable': ff['connections']['ENA'] = ff['connections']['DATAIN']
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
    inp = o / (name + '.json')
    inp.write_text(json.dumps(j))
    run(base + ['--json', inp, '--qsf', f / 'pins.qsf'], o / (name + '.log'), reason)
for command in ('set_output_delay', 'set_input_delay'):
    sdc = o / (command + '.sdc')
    sdc.write_text((f / 'clocks.sdc').read_text() + command + ' -max 2.0 [get_ports SDR_OUT]\n')
    run(base + ['--json', o / 'input.json', '--qsf', f / 'pins.qsf', '--sdc', sdc],
        o / (command + '.log'), "Unsupported SDC command '" + command + "'")
print('PASS disabled packing and seven unsupported requests')
