#!/usr/bin/env python3
"""Exercise physical boundaries, polarity, coverage and source retention."""
import copy
import gzip
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import reference as r


class NativeGpioReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest, cls.data = r.load()

    def test_complete_physical_coverage(self):
        self.assertEqual(len(self.data['binding']), 70)
        self.assertEqual(len(self.data['observations']), 1376)
        self.assertEqual(len(self.data['clock_observations']), 1120)
        self.assertEqual(len(r.envelopes(self.data)), 38)

    def test_independent_native_witnesses(self):
        values = r.envelopes(self.data)
        self.assertEqual(values['V12'][0], [[4, 468], [31, 469], [1734, 1905, 1830, 2010]])
        self.assertEqual(values['AE23'][0], [[1719, -83], [1755, -82], [2465, 4389, 2435, 4358]])

    def test_oe_inversion_preserves_native_input_edge(self):
        rows = [x for x in self.data['observations'] if x['role'] == 'OE']
        self.assertTrue(any(x['edge'] != x['query_edge'] for x in rows))
        self.assertTrue(all(x['rf'][-1] == x['data_cell_rf'][0] for x in rows))

    def test_report_quantities_are_finite_exact_picoseconds(self):
        self.assertEqual(r.ps('-0.083'), -83)
        for invalid in ('nan', 'inf', '1e3', '0.5', '-.010'):
            with self.assertRaises(ValueError):
                r.ps(invalid)

    def test_missing_data_corner_is_rejected(self):
        data = copy.deepcopy(self.data)
        data['observations'] = [x for x in data['observations'] if x['corner'] != r.CORNERS[-1]]
        with self.assertRaises(ValueError):
            r.validate(data)

    def test_missing_native_data_edge_is_rejected(self):
        data = copy.deepcopy(self.data)
        data['observations'] = [x for x in data['observations'] if not
                                (x['pin'] == 'AE23' and x['role'] == 'OE' and x['edge'] == 'fall')]
        with self.assertRaises(ValueError):
            r.validate(data)

    def test_duplicate_or_conflicting_q_probe_is_rejected(self):
        for defect in ('duplicate', 'conflict'):
            data = copy.deepcopy(self.data)
            row = next(x for x in data['observations'] if x['role'] == 'Q')
            if defect == 'duplicate':
                data['observations'].append(copy.deepcopy(row))
            else:
                row['value_ps'] += 20
            with self.assertRaises(ValueError):
                r.validate(data)

    def test_missing_clock_edge_is_rejected(self):
        data = copy.deepcopy(self.data)
        data['clock_observations'] = [x for x in data['clock_observations'] if x['edge'] != 'fall']
        with self.assertRaises(ValueError):
            r.validate(data)

    def test_changed_clock_frame_is_rejected(self):
        data = copy.deepcopy(self.data)
        for row in data['clock_observations']:
            row['value_ps'] += 100
        with self.assertRaises(ValueError):
            r.validate(data)

    def test_signed_setup_and_hold_are_not_clamped(self):
        values = r.envelopes(self.data)
        self.assertLess(values['AE23'][0][0][1], 0)
        self.assertTrue(any(x['role'] == 'I' and x['value_ps'] < 0 for x in self.data['observations']))

    def test_local_anchor_requires_exact_native_coordinate(self):
        row = next(x for x in self.data['observations'] if x['role'] == 'Q' and x['pin'] == 'V12')
        points = list(r.paths(gzip.decompress((r.ROOT/'reports'/(row['report']+'.gz')).read_bytes()).decode('latin1')))
        path = next(x for x in points if (x['source'], x['sink']) == (row['source'], row['sink']))
        configs = copy.deepcopy(self.data['configuration'])
        configs[row['pin']]['values']['DATAIN3'] = 'GIN.000.000.0000'
        with self.assertRaises(ValueError):
            r.extract(path, self.data['binding'], configs, row['report'])

    def test_missing_return_interconnect_is_rejected(self):
        row = next(x for x in self.data['observations'] if x['role'] == 'Q' and x['pin'] == 'V12')
        points = list(r.paths(gzip.decompress((r.ROOT/'reports'/(row['report']+'.gz')).read_bytes()).decode('latin1')))
        path = next(x for x in points if (x['source'], x['sink']) == (row['source'], row['sink']))
        points = path['points']
        q = next(i for i,p in enumerate(points) if p['type'] == 'CELL' and p['element'] == path['source']+'|q')
        buf = points.index(row['fabric_buffer'])
        del points[q+1:buf]
        with self.assertRaises(ValueError):
            r.extract(path, self.data['binding'], self.data['configuration'], row['report'])

    def test_changed_fitted_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'ref'
            shutil.copytree(r.ROOT, root, ignore=shutil.ignore_patterns('__pycache__'))
            (root/'fixture/clocks.sdc').write_text('create_clock -period 20 clk\n')
            with self.assertRaisesRegex(ValueError, 'Changed source'):
                r.load(root)

    def test_missing_raw_report_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = copy.deepcopy(self.manifest)
            manifest['reports_sha256'].pop(next(iter(manifest['reports_sha256'])))
            (root/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'Incomplete raw report inventory'):
                r.load(root)

    def test_wrong_operating_condition_or_query_is_rejected(self):
        name = 'native-clock/'+r.CORNERS[0]+'/rise-max.rpt'
        text = gzip.decompress((r.ROOT/'reports'/(name+'.gz')).read_bytes()).decode('latin1')
        for bad in (text.replace('Slow 1100mV 100C Model','Fast 1100mV 100C Model'),
                    text.replace('-rise_to','-fall_to'), text.replace('-show_routing','')):
            with self.assertRaises(ValueError):
                r.validate_report(name,bad)


if __name__ == '__main__':
    unittest.main()
