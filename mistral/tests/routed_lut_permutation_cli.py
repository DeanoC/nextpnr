#!/usr/bin/env python3
"""The lut-permutation post-route diagnostic only admits fresh full GPU execution."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY=sys.argv.pop(1)

class RoutedLutPermutationCli(unittest.TestCase):
    def run_case(self, options=(), step=None, settings=None, no_route=True):
        with tempfile.TemporaryDirectory(prefix='lut-permutation-route-cli-') as directory:
            root=Path(directory)
            module=dict(attributes={'top':1},ports={},cells={},netnames={})
            if step is not None:module['attributes']['step']=step
            if settings is not None:module['settings']=settings
            (root/'source.json').write_text(json.dumps({'modules':{'top':module}}))
            command=[BINARY,'--device','5CSEBA6U23I7','--json',str(root/'source.json'),'--write',str(root/'placed.json')]
            if no_route:command+=['--no-route']
            command+=list(options)
            result=subprocess.run(command,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
            output=(root/'placed.json').read_bytes() if (root/'placed.json').exists() else None
            self.assertFalse((root/'test.rbf').exists())
            return result,output

    def rejected(self,options,step=None,settings=None,no_route=True):
        result,output=self.run_case(options,step,settings,no_route)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Critical LUT permutation requires a fresh ordinary full GPU route',result.stdout)
        self.assertIsNone(output)

    def test_default_and_zero_are_byte_identical(self):
        before,a=self.run_case();after,b=self.run_case(['--critical-lut-permute-budget','0'])
        self.assertEqual(before.returncode,0,before.stdout);self.assertEqual(after.returncode,0,after.stdout)
        self.assertEqual(a,b);self.assertNotIn('Critical LUT permutation discovery',after.stdout)

    def test_budget_bounds(self):
        for budget in ('-1','5'):
            result,output=self.run_case(['--critical-lut-permute-budget',budget])
            self.assertNotEqual(result.returncode,0);self.assertIn('must be between 0 and 4',result.stdout);self.assertIsNone(output)

    def test_missing_full_gpu_route_rejects(self):
        for options in ([],['--rbf','test.rbf'],['--router','router2','--rbf','test.rbf']):
            self.rejected(['--critical-lut-permute-budget','1']+options)

    def test_reload_and_partial_execution_reject(self):
        base=['--critical-lut-permute-budget','1','--router','gpu','--rbf','test.rbf']
        for option in ('--no-pack','--no-place','--pack-only'):
            self.rejected(base+[option],no_route=False)
        self.rejected(base,step='route',no_route=False)

    def test_move_and_forced_timing_reject(self):
        base=['--critical-lut-permute-budget','1','--router','gpu','--rbf','test.rbf']
        self.rejected(base+['--critical-move-route-budget','1'],no_route=False)
        self.rejected(base+['--force'],no_route=False)
        self.rejected(base,settings={'timing/allowFail':'1'},no_route=False)

    def test_saved_setting_never_enables(self):
        result,output=self.run_case(settings={'critical-lut-permute-budget':'4'})
        self.assertEqual(result.returncode,0,result.stdout);self.assertIsNotNone(output)
        self.assertNotIn('Critical LUT permutation discovery',result.stdout)

if __name__=='__main__':unittest.main()
