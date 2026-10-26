#!/usr/bin/env python3
"""Native-cut equations and per-role coverage from actual fitted reports."""
import gzip
import json
from pathlib import Path
import unittest
from model_reference import ROOT, SYNC_DIN3, envelopes, load_reference, mode_key
from reference import fixture, parse_report


class NativeReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest, cls.data = load_reference()

    def test_native_clock_increment_is_included_once(self):
        text = gzip.decompress((ROOT/'witnesses/mlab-positive-sclr/7_slow_1100mv_100c/din-setup-fall.rpt.gz').read_bytes()).decode('latin1')
        rows = parse_report(text, fixture(ROOT,'mlab-positive-sclr'), 'din','setup','fall',
                            '7_slow_1100mv_100c',native_cuts=True)
        row = next(r for r in rows if r['cell']=='sink_2_6_1')
        self.assertEqual(row['effective_pin_constraint_ns'],.081)
        self.assertEqual(row['effective_native_constraint_ns'],.080)
        self.assertEqual(row['native_clock_local_ns'],.504)
        self.assertEqual(row['native_data_local_ns'],.584)

    def test_inactive_controls_have_no_report_endpoint(self):
        spec = fixture(ROOT,'native-lab-mixed-positive-sync')
        text = gzip.decompress((ROOT/'witnesses/native-mixed-sync.rpt.gz').read_bytes()).decode('latin1')
        rows = parse_report(text,spec,'sclear','setup','rise','7_slow_1100mv_100c',native_cuts=True)
        stored = [r for r in self.data['cases']['native-lab-mixed-positive-sync']['observations']
                  if r['role']=='sclear' and r['analysis']=='setup' and r['edge']=='rise' and r['corner']=='7_slow_1100mv_100c']
        self.assertEqual({r['cell']:r for r in rows},{r['cell']:r for r in stored})
        expected = {s['cell'] for s in spec['sites'] if s['synchronous_clear_enabled']}
        self.assertEqual(len(expected),72)
        self.assertEqual({r['cell'] for r in rows},expected)
        # A metadata omission must not silently narrow the reference.
        changed = dict(spec,sites=[s for s in spec['sites'] if s['cell']!=rows[0]['cell']])
        with self.assertRaises(AssertionError):
            parse_report(text,changed,'sclear','setup','rise','7_slow_1100mv_100c',native_cuts=True)

    def test_missing_corner_transition_is_not_a_bound(self):
        case = self.data['cases']['native-lab-constant-positive-plain']
        config = case['configuration'][0]
        rows = [r for r in case['observations'] if r['cell']==config['cell']]
        single = dict(cases={'case':dict(configuration=[config],observations=rows)})
        envelopes(single)
        bad = [r for r in rows if not (r['role']=='Q' and r['analysis']=='hold' and r['edge']=='fall')]
        with self.assertRaises(AssertionError):
            envelopes(dict(cases={'case':dict(configuration=[config],observations=bad)}))

    def test_outward_rounding_and_signed_requirements(self):
        values = envelopes(self.data)
        self.assertTrue(any(role[0] < 0 for corners in values.values() for roles in corners for role in roles[1:] if role))
        case = self.data['cases']['native-lab-constant-positive-plain']
        config = case['configuration'][0]
        rows = [r for r in case['observations'] if r['cell']==config['cell']]
        table = envelopes(dict(cases={'case':dict(configuration=[config],observations=rows)}))[config['mode_key']]
        selected = [r for r in rows if r['corner']=='7_slow_1100mv_100c' and r['role']=='Q' and r['edge']=='rise']
        self.assertEqual(table[0][0][0],round(min(r['clock_anchor_to_q_ns'] for r in selected if r['analysis']=='hold')*1000)-1)
        self.assertEqual(table[0][0][1],round(max(r['clock_anchor_to_q_ns'] for r in selected if r['analysis']=='setup')*1000)+1)

    def test_synchronous_clear_selectors_have_separate_modes(self):
        name = 'native-lab-constant-positive-sync'
        spec = fixture(ROOT,name)
        sites = {s['cell']:s for s in spec['sites']}
        source = (ROOT/'fixtures'/name/'top.sv').read_text()
        config = next(c for c in self.data['cases'][name]['configuration'] if c['settings']['SCLR_SEL']=='DIN1')
        changed = dict(config,settings=dict(config['settings'],SCLR_SEL='DIN3'))
        self.assertEqual(mode_key(config,sites[config['cell']],source)^mode_key(changed,sites[config['cell']],source),SYNC_DIN3)

    def test_ef_data_selectors_do_not_share_a_role(self):
        name = 'native-lab-constant-positive-plain'
        case = self.data['cases'][name]
        config = next(c for c in case['configuration'] if c['PKREG']==1)
        rows = [r for r in case['observations'] if r['cell']==config['cell']]
        roles = envelopes(dict(cases={'case':dict(configuration=[config],observations=rows)}))[config['mode_key']][0]
        choice = next(v for k,v in config['settings'].items() if k in ('TEF_SEL','BEF_SEL'))
        self.assertIsNotNone(roles[2 if choice=='E' else 3])
        self.assertIsNone(roles[3 if choice=='E' else 2])


if __name__=='__main__':
    unittest.main(verbosity=2)
