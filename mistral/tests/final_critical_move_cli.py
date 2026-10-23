#!/usr/bin/env python3
"""Draft CPU-only admission controls; no invocation in this file can start a route."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


class FinalCriticalMoveCli(unittest.TestCase):
    def run_case(self, options=(), step=None, settings=None, no_route=True):
        with tempfile.TemporaryDirectory(prefix='final-critical-move-cli-') as directory:
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
            command += list(options)
            result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=60)
            output = (root / 'placed.json').read_bytes() if (root / 'placed.json').exists() else None
            self.assertFalse((root / 'test.rbf').exists())
            return result, output

    def rejected(self, options, step=None, settings=None, no_route=True):
        result, output = self.run_case(options, step, settings, no_route)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Final critical move requires a fresh ordinary full GPU route', result.stdout)
        self.assertIsNone(output)

    def test_default_and_zero_are_byte_identical(self):
        before, a = self.run_case()
        after, b = self.run_case(['--critical-final-move-budget', '0'])
        self.assertEqual(before.returncode, 0, before.stdout)
        self.assertEqual(after.returncode, 0, after.stdout)
        self.assertEqual(a, b)
        self.assertNotIn('Final critical move repair:', after.stdout)

    def test_bounds(self):
        for value in ('-1', '17'):
            result, output = self.run_case(['--critical-final-move-budget', value])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('must be between 0 and 16', result.stdout)
            self.assertIsNone(output)

    def test_requires_explicit_lut_pass(self):
        for options in ([], ['--critical-lut6-swap-budget', '0'], ['--critical-route-budget', '0']):
            result, output = self.run_case(['--critical-final-move-budget', '1'] + options)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires a positive explicit preceding LUT or final-route repair budget', result.stdout)
            self.assertIsNone(output)

    def test_requires_fresh_full_gpu_execution(self):
        for value in ('1', '8', '16'):
            self.rejected(['--critical-final-move-budget', value, '--critical-lut6-swap-budget', '1'])
        for options in (['--rbf', 'test.rbf'], ['--rbf', 'test.rbf', '--router', 'router2']):
            self.rejected(['--critical-final-move-budget', '1', '--critical-lut-permute-budget', '1'] + options)

    def test_reload_and_partial_execution_reject(self):
        base = ['--critical-final-move-budget', '1', '--critical-lut6-swap-budget', '1',
                '--router', 'gpu', '--rbf', 'test.rbf']
        for option in ('--no-pack', '--no-place', '--pack-only'):
            self.rejected(base + [option], no_route=False)
        self.rejected(base, step='route', no_route=False)

    def test_forced_timing_and_move_reject(self):
        base = ['--critical-final-move-budget', '1', '--critical-private-pair-budget', '1',
                '--router', 'gpu', '--rbf', 'test.rbf']
        self.rejected(base + ['--force'], no_route=False)
        self.rejected(base + ['--critical-move-route-budget', '1'], no_route=False)
        self.rejected(base, settings={'timing/allowFail': '1'}, no_route=False)

    def test_saved_settings_do_not_enable(self):
        result, output = self.run_case(settings={'critical-final-move-budget': '16'})
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIsNotNone(output)
        self.assertNotIn('Final critical move repair:', result.stdout)


if __name__ == '__main__':
    unittest.main()
