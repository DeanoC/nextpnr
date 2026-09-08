#!/usr/bin/env python3
"""Host-only checks for GP staging, including a late selector transition."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

SELECT = 1 << 26
CONTROLS = 0xf0000000
PAYLOAD = 0x03ff03ff
ARM = 0x13579bdf


def safe_transition(old, new):
    if (old ^ new) & SELECT:
        assert not ((old ^ new) & PAYLOAD), 'selector and payload changed together'
        assert not ((old | new) & CONTROLS), 'selector changed with controls active'
    if (old ^ new) & PAYLOAD:
        assert not ((old | new) & CONTROLS), 'payload changed with controls active'


class Model:
    def __init__(self, unit, lanes):
        self.unit, self.lanes = unit, lanes
        self.mask = (1 << unit) - 1
        self.memory = [((i * 73) ^ (i >> 1) ^ 0xa6) & self.mask for i in range(1024)]
        self.address, self.seed, self.q = [0, 0], [0, 0], [0, 0]
        self.armed = False
        self.last = 0

    def tick(self, word):
        port = (word >> 26) & 1
        self.address[port] = word & (1024 // self.lanes[port] - 1)
        self.seed[port] = (word >> 16) & self.mask
        self.armed |= word == ARM
        for p in (0, 1):
            if not (word & (1 << (28 + p))):
                continue
            start = self.address[p] * self.lanes[p]
            if self.armed and (word & (1 << (30 + p))):
                self.memory[start:start + self.lanes[p]] = [
                    (self.seed[p] ^ ((i + 2 * p) * 0x93)) & self.mask
                    for i in range(self.lanes[p])]
            self.q[p] = sum(self.memory[start + i] << (self.unit * i)
                            for i in range(self.lanes[p]))

    def put(self, word):
        if (self.last ^ word) & SELECT:
            # Worst case: new payload/control bits arrive before the selector;
            # the departing port captures them for a clock edge.
            self.tick((word & ~SELECT) | (self.last & SELECT))
        self.tick(word)
        self.last = word

    def output(self):
        return (self.q[(self.last >> 26) & 1] >> (16 * ((self.last >> 27) & 1))) & 65535


class ProbeTest(unittest.TestCase):
    def test_original_simultaneous_write_transition_is_unsafe(self):
        old, new = 0x04c84048, 0xf0214020
        with self.assertRaisesRegex(AssertionError, 'selector and payload'):
            safe_transition(old, new)
        model = Model(8, (2, 1))
        model.armed = True
        model.put(old)
        model.put(new)
        self.assertEqual(model.memory[72], 0x0a)  # Intended B write missed.
        self.assertEqual(model.address[1], 32)  # Captured departing bus payload.

    def test_generated_sequences(self):
        probe = Path(__file__).with_name('true_dual_port_mixed_probe.py')
        for unit in (8, 10):
            for alanes in (1, 2):
                for blanes in (1, 2):
                    with self.subTest(unit=unit, a=alanes, b=blanes):
                        shell = subprocess.check_output([
                            sys.executable, str(probe), '--unit', str(unit),
                            '--a-lanes', str(alanes), '--b-lanes', str(blanes)], text=True)
                        subprocess.run(['sh', '-n'], input=shell, text=True, check=True)
                        model = Model(unit, (alanes, blanes))
                        checks = 0
                        for line in shell.splitlines():
                            if re.fullmatch(r'put (?:0x[0-9a-f]+|[0-9]+)', line):
                                word = int(line.split()[1], 0)
                                if word != ARM:
                                    safe_transition(model.last, word)
                                model.put(word)
                            elif re.fullmatch(r'check [0-9]+', line):
                                self.assertEqual(model.output(), int(line.split()[1]))
                                checks += 1
                        self.assertGreater(checks, 100)


if __name__ == '__main__':
    unittest.main()
