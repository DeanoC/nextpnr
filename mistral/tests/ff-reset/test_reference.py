#!/usr/bin/env python3
import gzip
import hashlib
import json
import tempfile
from pathlib import Path
import unittest
from summarize import CORNERS, clear_sources, parse_report, source_digest


class ReferenceTest(unittest.TestCase):
    def setUp(self):
        self.site = dict(cell='sink', location='FF_X31_Y49_N1', clock='clock',
                         alm=0, ff_index=0, dynamic_enable=False, enable_inverted=False,
                         clear_source='reset_n')
        self.expected = {'sink': self.site}

    def report(self, kind='recovery', data=.623, clock=.489):
        intrinsic = 'uTsu' if kind == 'recovery' else 'uTh'
        return f'''\nPath #1: {kind}
; From Node ; reset_n ;
; To Node ; sink ;
; Launch Clock ; clock ;
; Latch Clock ; clock ;
; Slack ; 1.0 ;
; Data Arrival Path
; 0 ; 0.4 ; RR ; IC ; 1 ; FF_X31_Y49_N1 ; sink|clrn ;
; 0 ; {data} ; RF ; CELL ; 1 ; FF_X31_Y49_N1 ; sink ;
; Data Required Path
; 0 ; 0.4 ; RR ; IC ; 1 ; FF_X31_Y49_N1 ; sink|clk ;
; 0 ; {clock} ; RR ; CELL ; 1 ; FF_X31_Y49_N1 ; sink ;
; 0 ; 0 ; ; {intrinsic} ; 1 ; FF_X31_Y49_N1 ; sink ;
'''

    def test_recovery_normalizes_clear_against_clock(self):
        row = parse_report(self.report(), 'recovery', 'corner', self.expected)[0]
        self.assertEqual(row['local_requirement_ns'], .134)

    def test_removal_reverses_subtraction(self):
        row = parse_report(self.report('removal', .508, .579), 'removal', 'corner', self.expected)[0]
        self.assertEqual(row['local_requirement_ns'], .071)

    def test_missing_or_duplicate_endpoint_rejected(self):
        with self.assertRaises(AssertionError):
            parse_report(self.report(), 'recovery', 'corner', dict(self.expected, missing=self.site))
        with self.assertRaises(AssertionError):
            parse_report(self.report() * 2, 'recovery', 'corner', self.expected)

    def test_wrong_location_or_clock_rejected(self):
        for original, replacement in [('FF_X31_Y49_N1', 'FF_X31_Y49_N2'),
                                      ('Launch Clock ; clock', 'Launch Clock ; other')]:
            with self.subTest(original=original), self.assertRaises(AssertionError):
                parse_report(self.report().replace(original, replacement), 'recovery', 'corner', self.expected)

    def test_wrong_clear_source_or_deassertion_transition_rejected(self):
        with self.assertRaises(AssertionError):
            parse_report(self.report().replace('; From Node ; reset_n ;', '; From Node ; other_reset ;'),
                         'recovery', 'corner', self.expected)
        with self.assertRaises(AssertionError):
            parse_report(self.report().replace('; 0 ; 0.4 ; RR ; IC ; 1 ; FF_X31_Y49_N1 ; sink|clrn ;',
                                               '; 0 ; 0.4 ; RF ; IC ; 1 ; FF_X31_Y49_N1 ; sink|clrn ;'),
                         'recovery', 'corner', self.expected)

    def test_missing_intrinsic_and_nonfinite_values_rejected(self):
        with self.assertRaises(AssertionError):
            parse_report(self.report().replace('; uTsu ;', '; absent ;'), 'recovery', 'corner', self.expected)
        with self.assertRaises(AssertionError):
            parse_report(self.report(data=float('nan')), 'recovery', 'corner', self.expected)

    def test_retained_reference_matches_sources_and_model_limits(self):
        root = Path(__file__).resolve().parent
        archive = (root / 'reference.json.gz').read_bytes()
        raw = gzip.decompress(archive)
        data = json.loads(raw)
        summary = json.loads((root / 'reference-summary.json').read_text())
        self.assertEqual(hashlib.sha256(archive).hexdigest(), summary['full_reference_gzip_sha256'])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), summary['full_reference_json_sha256'])
        total = 0
        for name, project in data['projects'].items():
            evidence = project['evidence']
            fixture = root / 'fixtures' / name
            clear_by_cell = clear_sources(fixture / 'top.sv')
            self.assertEqual(set(clear_by_cell),
                             {v['cell'] for v in json.loads((fixture / 'fixture.json').read_text())['sites']})
            for cell, source in clear_by_cell.items():
                expected_source = f'reset_n_{cell.split("_")[1]}' if name == 'rates' else 'reset_n'
                self.assertEqual(source, expected_source)
            for filename, digest in evidence['source_sha256'].items():
                self.assertEqual(source_digest(fixture / filename), digest)
            expected = {v['cell'] for v in json.loads((fixture / 'fixture.json').read_text())['sites']}
            rows = evidence['observations']
            self.assertEqual(len(rows), len(expected) * len(CORNERS) * 2)
            total += len(rows)
            for corner in CORNERS:
                for kind in ('recovery', 'removal'):
                    selected = [v for v in rows if v['corner'] == corner and v['kind'] == kind]
                    self.assertEqual(len(selected), len(expected))
                    self.assertEqual({v['cell'] for v in selected}, expected)
                    self.assertTrue(all(v['fitted_slack_ns'] >= 0 for v in selected))
            for kind in ('recovery', 'removal'):
                maximum = max(v['local_requirement_ns'] for v in rows if v['kind'] == kind)
                self.assertEqual(maximum, evidence['observed_max_ns'][kind])
                self.assertLessEqual(maximum * 1000, data['model_ps'][kind])
        self.assertEqual(total, 1648)
        self.assertEqual(data['model_ps'], {'recovery': 170, 'removal': 270})

    def test_qsf_identity_ignores_only_known_tool_bookkeeping(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'top.qsf'
            base = 'set_global_assignment -name DEVICE 5CSEBA6U23I7\n'
            path.write_text(base)
            value = source_digest(path)
            path.write_text(base + '\nset_global_assignment -name LAST_QUARTUS_VERSION "17.0.2 Lite Edition"\n')
            self.assertEqual(source_digest(path), value)
            path.write_text(base.replace('I7', 'C6'))
            self.assertNotEqual(source_digest(path), value)


if __name__ == '__main__':
    unittest.main()
