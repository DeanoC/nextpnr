#!/usr/bin/env python3
"""Compare startup, clear and enable sequences before/after SDR output packing."""
import argparse
import itertools
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('yosys', 'nextpnr', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
a = p.parse_args()
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent

def run(cmd, name):
    with (o / (name + '.log')).open('w') as log:
        subprocess.run([str(x) for x in cmd], stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=180)

run([a.yosys.resolve(), '-p', f'read_verilog {here / "controls.v"}; '
     f'synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {o / "input.json"}'], 'synth')
(o / 'pins.qsf').write_text((here / 'pins.qsf').read_text() +
    'set_location_assignment PIN_W14 -to CLEAR\n'
    'set_location_assignment PIN_AF4 -to ENABLE\n')
run([a.nextpnr.resolve(), '--device', '5CSEBA6U23I7', '--json', o / 'input.json',
     '--qsf', o / 'pins.qsf', '--sdc', here / 'clocks.sdc', '--pack-only',
     '--write', o / 'packed.json'], 'pack')

class Circuit:
    def __init__(self, path, kind):
        self.module = json.loads(path.read_text())['modules']['top']
        self.cells = self.module['cells']
        self.reg = next(c for c in self.cells.values() if c['type'] == kind)
        self.state = int(self.reg['parameters'].get('IOREG_OUT_POWER_UP', '0'), 2)
        self.driver = {bit: (c, port) for c in self.cells.values()
                       for port, bits in c['connections'].items()
                       if c['port_directions'][port] == 'output' for bit in bits}
        self.values = {}

    def set_inputs(self, values):
        self.values = {self.module['ports'][name]['bits'][0]: value
                       for name, value in values.items()}

    def value(self, bit):
        if bit in ('0', '1'): return int(bit)
        if bit in self.values: return self.values[bit]
        cell, port = self.driver[bit]
        t, conn = cell['type'], cell['connections']
        def get(name): return self.value(conn[name][0])
        if cell is self.reg: return self.state
        if t in ('MISTRAL_IB', 'MISTRAL_OB'): return get('PAD' if t == 'MISTRAL_IB' else 'I')
        if t == 'MISTRAL_NOT': return 1 - get('A')
        if t in ('MISTRAL_BUF', 'MISTRAL_CLKBUF'): return get('A')
        if t in ('GND', 'VCC'): return int(t == 'VCC')
        if t == 'MISTRAL_CONST': return int(cell['parameters']['LUT'], 2)
        if t.startswith('MISTRAL_ALUT'):
            index = sum(get(name) << i for i, name in enumerate('ABCDEF') if name in conn)
            return (int(cell['parameters']['LUT'], 2) >> index) & 1
        raise ValueError((t, port))

    def edge(self):
        c = self.reg['connections']
        def get(name): return self.value(c[name][0])
        if self.reg['type'] == 'MISTRAL_FF':
            if not get('ACLR'): self.state = 0
            elif get('ENA'):
                self.state = 0 if get('SCLR') else get('SDATA') if get('SLOAD') else get('DATAIN')
        else:
            enable = 'CEOUT' not in c or (get('CEOUT') ^ int(self.reg['parameters'].get('IOREG_CEOUT_INV', '0'), 2))
            if enable: self.state = get('I')

    def output(self):
        return self.value(self.module['ports']['SDR_OUT']['bits'][0])

before, after = Circuit(o / 'input.json', 'MISTRAL_FF'), Circuit(o / 'packed.json', 'MISTRAL_SDROUT')
values = dict(FPGA_CLK1_50=0, DATA=0, CLEAR=0, ENABLE=0)
for circuit in (before, after): circuit.set_inputs(values)
assert before.output() == after.output() == 1, 'startup polarity'
# All two-cycle input combinations exercise retained state when enable is low,
# including a synchronous clear asserted during a disabled edge.
cases = list(itertools.product((0, 1), repeat=3))
for pair in itertools.product(cases, repeat=2):
    for data, clear, enable in pair:
        values.update(DATA=data, CLEAR=clear, ENABLE=enable)
        for circuit in (before, after): circuit.set_inputs(values); circuit.edge()
        assert before.output() == after.output(), (values, before.output(), after.output())
print('PASS startup and 128 clear/enable/data edges match the synthesized FF circuit')
