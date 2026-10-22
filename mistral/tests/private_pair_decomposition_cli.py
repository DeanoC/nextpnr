#!/usr/bin/env python3
"""Explicit fresh-placement private-pair opt-in and fail-closed selection."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)

class PrivatePairCli(unittest.TestCase):
    def run_case(self, options=(), step=None, settings=None, report=True):
        with tempfile.TemporaryDirectory(prefix='private-pair-cli-') as directory:
            root = Path(directory)
            module = dict(attributes={'top':1},ports={},cells={},netnames={})
            if step is not None: module['attributes']['step'] = step
            if settings is not None: module['settings'] = settings
            (root/'source.json').write_text(json.dumps({'modules':{'top':module}}))
            (root/'report.json').write_text(json.dumps({'critical_paths':[]}))
            command = [BINARY,'--device','5CSEBA6U23I7','--json',str(root/'source.json'),
                       '--no-route','--write',str(root/'placed.json')]
            if report: command += ['--remap-private-pair-critical',str(root/'report.json')]
            command += list(options)
            result = subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
            output = (root/'placed.json').read_bytes() if (root/'placed.json').exists() else None
            return result,output

    def test_candidate_requires_report(self):
        result,output = self.run_case(['--remap-private-pair-candidate','0'],report=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('requires --remap-private-pair-critical',result.stdout)
        self.assertIsNone(output)

    def test_empty_listing_preserves_default_checkpoint(self):
        before,a = self.run_case(report=False)
        after,b = self.run_case()
        self.assertEqual(before.returncode,0,before.stdout)
        self.assertEqual(after.returncode,0,after.stdout)
        self.assertEqual(a,b)
        self.assertIn('0 qualified candidates',after.stdout)

    def test_missing_candidate_emits_no_checkpoint(self):
        result,output = self.run_case(['--remap-private-pair-candidate','0'])
        self.assertNotEqual(result.returncode,0)
        self.assertIn('was not qualified',result.stdout)
        self.assertIsNone(output)

    def test_negative_selection_rejects(self):
        result,output = self.run_case(['--remap-private-pair-candidate','-2'])
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Invalid private pair candidate index',result.stdout)
        self.assertIsNone(output)

    def test_reloaded_and_saved_sa_modes_reject(self):
        for options,step,settings in ((['--no-pack','--no-place'],'place',None),
                                      ([],None,{'placer':'sa'})):
            result,output = self.run_case(options,step,settings)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('fresh ordinary HeAP placement',result.stdout)
            self.assertIsNone(output)

    def test_other_placement_rewrite_rejects(self):
        result,output = self.run_case(['--replicate-enables','0'])
        self.assertNotEqual(result.returncode,0)
        self.assertIn('another placement rewrite',result.stdout)
        self.assertIsNone(output)

    def test_saved_settings_do_not_enable_pass(self):
        result,output = self.run_case(settings={'remap-private-pair-critical':'missing','remap-private-pair-candidate':'0'},report=False)
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertIsNotNone(output)
        self.assertNotIn('Private pair discovery',result.stdout)

if __name__ == '__main__': unittest.main()
