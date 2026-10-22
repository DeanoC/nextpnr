#!/usr/bin/env python3
"""Explicit unused-arithmetic coverage only accompanies an eligible live repair."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)
FLAG = '--critical-unobserved-arith-outputs'

class UnobservedArithmeticCli(unittest.TestCase):
    def run_case(self, options=(), step=None, settings=None, no_route=True):
        with tempfile.TemporaryDirectory(prefix='unobserved-arith-cli-') as directory:
            root = Path(directory)
            module = dict(attributes={'top': 1}, ports={}, cells={}, netnames={})
            if step is not None:
                module['attributes']['step'] = step
            if settings is not None:
                module['settings'] = settings
            (root / 'source.json').write_text(json.dumps({'modules': {'top': module}}))
            command = [BINARY, '--device', '5CSEBA6U23I7', '--json', str(root / 'source.json'),
                       '--write', str(root / 'placed.json')]
            if no_route:
                command.append('--no-route')
            result = subprocess.run(command + list(options), cwd=root, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=60)
            output = (root / 'placed.json').read_bytes() if (root / 'placed.json').exists() else None
            self.assertFalse((root / 'test.rbf').exists())
            return result, output

    def rejected(self, options, text, **kwargs):
        result, output = self.run_case(options, **kwargs)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(text, result.stdout)
        self.assertIsNone(output)

    def test_flag_requires_positive_repair_budget(self):
        for options in ([], ['--critical-private-pair-budget', '0'],
                        ['--critical-lut-permute-budget', '0']):
            self.rejected([FLAG] + options, 'requires a positive private-pair or LUT-permutation budget')

    def test_lut6_only_cannot_enable_optional_coverage(self):
        self.rejected([FLAG, '--critical-lut6-swap-budget', '1', '--router', 'gpu', '--rbf', 'test.rbf'],
                      'requires a positive private-pair or LUT-permutation budget', no_route=False)

    def test_each_consumer_retains_full_gpu_requirement(self):
        for budget, name in (('--critical-private-pair-budget', 'Critical private pair'),
                             ('--critical-lut-permute-budget', 'Critical LUT permutation')):
            for options in ([], ['--rbf', 'test.rbf'], ['--router', 'router2', '--rbf', 'test.rbf']):
                self.rejected([FLAG, budget, '1'] + options, name + ' requires a fresh ordinary full GPU route')

    def test_reload_partial_and_forced_reject(self):
        for budget, name in (('--critical-private-pair-budget', 'Critical private pair'),
                             ('--critical-lut-permute-budget', 'Critical LUT permutation')):
            base = [FLAG, budget, '1', '--router', 'gpu', '--rbf', 'test.rbf']
            text = name + ' requires a fresh ordinary full GPU route'
            for option in ('--no-pack', '--no-place', '--pack-only', '--force'):
                self.rejected(base + [option], text, no_route=False)
            self.rejected(base, text, step='route', no_route=False)
            self.rejected(base, text, settings={'timing/allowFail': '1'}, no_route=False)

    def test_move_repair_cannot_combine(self):
        for budget, name in (('--critical-private-pair-budget', 'Critical private pair'),
                             ('--critical-lut-permute-budget', 'Critical LUT permutation')):
            self.rejected([FLAG, budget, '1', '--router', 'gpu', '--rbf', 'test.rbf',
                           '--critical-move-route-budget', '1'],
                          name + ' requires a fresh ordinary full GPU route', no_route=False)

    def test_default_zero_and_saved_setting_do_not_enable(self):
        baseline, a = self.run_case()
        self.assertEqual(baseline.returncode, 0, baseline.stdout)
        zero, b = self.run_case(['--critical-private-pair-budget', '0', '--critical-lut-permute-budget', '0'])
        self.assertEqual(zero.returncode, 0, zero.stdout)
        self.assertEqual(a, b)
        saved, output = self.run_case(settings={'critical-unobserved-arith-outputs': '1'})
        self.assertEqual(saved.returncode, 0, saved.stdout)
        self.assertIsNotNone(output)
        self.assertNotIn('requires a positive private-pair', saved.stdout)
        self.assertNotIn('Critical LUT permutation discovery', saved.stdout)
        self.assertNotIn('Critical routed private pair discovery', saved.stdout)

if __name__ == '__main__':
    unittest.main()
