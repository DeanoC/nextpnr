#!/usr/bin/env python3
"""Regressions against actual fitted reports and immutable FF artifacts."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock
import characterize
from reference import check_config, digest, fixture, parse_report, validate_cached

ROOT = Path(__file__).resolve().parent
CONFIG_READER = None


class ReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / 'manifest.json').read_text())
        cls.raw = gzip.decompress((ROOT / 'reference.json.gz').read_bytes())
        cls.reference = json.loads(cls.raw)

    def witness(self, index):
        w = self.manifest['raw_report_witnesses'][index]
        text = gzip.decompress((ROOT / w['path']).read_bytes()).decode('latin1')
        report = w['path'].replace('witnesses/', 'reports/', 1)
        self.assertEqual(digest(ROOT / w['path']),
                         self.manifest['cases'][w['case']]['reports_sha256'][report])
        return w, text, fixture(ROOT, w['case'])

    def parse(self, witness, text, spec):
        return parse_report(text, spec, witness['role'], witness['analysis'], witness['edge'], witness['corner'])

    def test_complete_reference_matches_inputs_and_axes(self):
        summary = json.loads((ROOT / 'reference-summary.json').read_text())
        self.assertEqual(digest(ROOT / 'reference.json.gz'), summary['full_reference_gzip_sha256'])
        self.assertEqual(hashlib.sha256(self.raw).hexdigest(), summary['full_reference_json_sha256'])
        self.assertEqual(validate_cached(ROOT, self.reference), 101120)
        self.assertFalse(self.reference['hardware'])
        self.assertFalse(self.reference['physical_qualified'])

    def test_real_reports_cover_all_declared_physical_cells(self):
        for i in range(len(self.manifest['raw_report_witnesses'])):
            w, text, spec = self.witness(i)
            with self.subTest(witness=w):
                rows = self.parse(w, text, spec)
                self.assertEqual(len(rows), 120)
                expected = {(r['cell'], r['role'], r['analysis'], r['edge'], r['corner']): r
                            for r in self.reference['cases'][w['case']]['observations']}
                for row in rows:
                    self.assertEqual(row, expected[(row['cell'], row['role'], row['analysis'], row['edge'], row['corner'])])

    def test_missing_duplicate_and_wrong_location_rejected(self):
        w, text, spec = self.witness(4)
        marker = list(re.finditer(r'\nPath #\d+:', text))
        first = text[marker[0].start():marker[1].start()]
        bad = [text[:marker[0].start()] + text[marker[1].start():], text + first,
               text.replace(spec['sites'][0]['location'], 'FF_X99_Y99_N1')]
        for report in bad:
            with self.assertRaises(AssertionError):
                self.parse(w, report, spec)

    def test_clock_polarity_and_control_pin_identity_rejected(self):
        w, text, spec = self.witness(4)
        for report in (text.replace(' (INVERTED)', ''), text.replace('|sclr', '|ena')):
            with self.assertRaises(AssertionError):
                self.parse(w, report, spec)

    def test_missing_intrinsic_and_nonfinite_rejected(self):
        w, text, spec = self.witness(4)
        missing = re.sub(r';\s*uTsu\s*;', '; missing_intrinsic ;', text)
        with self.assertRaises(AssertionError):
            self.parse(w, missing, spec)
        # A real local data CELL row, not a fabricated report format.
        bad = re.sub(r'(?m)(^;\s*[-\d.]+\s*;)\s*[-\d.]+(\s*;\s*\w+\s*;\s*CELL\s*;[^\n]*\|sclr[^\n]*)', r'\1 nan\2', text)
        if bad == text:
            bad = re.sub(r'(?m)(^;\s*[-\d.]+\s*;)\s*[-\d.]+(\s*;\s*\w+\s*;\s*CELL\s*;[^\n]*sink_[^|\n]+;)', r'\1 nan\2', text, count=1)
        self.assertNotEqual(bad, text)
        with self.assertRaises(AssertionError):
            self.parse(w, bad, spec)

    def test_clear_deassertion_and_negative_fixture_slack_retained(self):
        w, text, spec = self.witness(5)
        rows = self.parse(w, text, spec)
        self.assertTrue(any(r['fitted_violation'] and r['fitted_slack_ns'] < 0 for r in rows))
        bad = re.sub(r'(;\s*)[RF]R(\s*;\s*IC\s*;[^\n]*\|clrn\s*;)', r'\1FF\2', text)
        self.assertNotEqual(bad, text)
        with self.assertRaises(AssertionError):
            self.parse(w, bad, spec)

    def test_cache_does_not_accept_narrowed_or_misidentified_rows(self):
        reference = dict(self.reference, cases=dict(self.reference['cases']))
        name = next(iter(reference['cases']))
        case = reference['cases'][name]
        for rows in (case['observations'][:-1], [dict(case['observations'][0], location='FF_X99_Y99_N1'), *case['observations'][1:]]):
            reference['cases'][name] = dict(case, observations=rows)
            with self.assertRaises(AssertionError):
                validate_cached(ROOT, reference)

    def test_characterizer_preserves_existing_and_failed_captures(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch('characterize.subprocess.check_output', return_value='Version 17.0.2 Build 602'):
            existing = Path(tmp) / 'existing'
            existing.mkdir()
            sentinel = existing / 'retained'
            sentinel.write_text('original')
            with self.assertRaises(FileExistsError):
                characterize.characterize(ROOT, existing, Path('/oracle'), Path('/reader'))
            self.assertEqual(sentinel.read_text(), 'original')
            failed = Path(tmp) / 'failed'
            reader = Path(tmp) / 'reader'
            reader.write_bytes(b'configuration-reader-test-placeholder')
            with mock.patch('characterize.subprocess.run', return_value=mock.Mock(returncode=1)):
                with self.assertRaises(RuntimeError):
                    characterize.characterize(ROOT, failed, Path('/oracle'), reader)
            self.assertTrue(list(failed.glob('*/input-binding.json')))
            self.assertEqual(json.loads(next(failed.glob('*/compile.log.receipt.json')).read_text())['returncode'], 1)
            self.assertFalse((failed / 'reference.json.gz').exists())

    def test_changed_fitted_control_configuration_rejected(self):
        if not CONFIG_READER:
            self.skipTest('requires built Mistral config reader')
        name = 'lab-positive'
        spec = fixture(ROOT, name)
        rows = self.reference['cases'][name]['observations']
        with tempfile.TemporaryDirectory() as tmp:
            rbf = Path(tmp) / 'fitted.rbf'
            rbf.write_bytes(gzip.decompress((ROOT / 'fixtures' / name / 'fitted.rbf.gz').read_bytes()))
            from subprocess import run
            request = ''.join(f'{s["location"].split("_")[1][1:]} {s["location"].split("_")[2][1:]} {s["alm"]} {s["ff_index"]}\n'
                              for s in spec['sites'])
            decoded = run([str(CONFIG_READER), str(rbf)], input=request, text=True,
                          capture_output=True, check=True).stdout
            for old, new in [('EN2_EN=1', 'EN2_EN=0'), ('EN2_NINV=0', 'EN2_NINV=1'),
                             ('CLK2_INV=0', 'CLK2_INV=1'), ('SCLR_INV=0', 'SCLR_INV=1')]:
                self.assertIn(old, decoded)
                with self.subTest(field=old), mock.patch('reference.subprocess.run',
                        return_value=mock.Mock(stdout=decoded.replace(old, new, 1))):
                    with self.assertRaises(AssertionError):
                        check_config(CONFIG_READER, rbf, spec, rows)
            with mock.patch('reference.subprocess.run', return_value=mock.Mock(stdout=decoded)):
                with self.assertRaises(AssertionError):
                    check_config(CONFIG_READER, rbf, dict(spec, expected_clock_indices=[0, 1, 2]), rows)

    def test_every_retained_RBF_matches_clock_and_input_identities(self):
        if not CONFIG_READER:
            self.skipTest('requires built Mistral config reader')
        self.assertEqual(validate_cached(ROOT, self.reference, CONFIG_READER), 101120)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--config-reader', type=Path)
    args, rest = parser.parse_known_args()
    CONFIG_READER = args.config_reader.resolve() if args.config_reader else None
    unittest.main(argv=[__file__, *rest], verbosity=2)
