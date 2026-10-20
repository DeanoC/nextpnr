#!/usr/bin/env python3
"""Native STA -> complete export -> retention decision and artifact transaction."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('guard', Path(__file__).resolve().parents[1] / 'routed_swap.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
BINARY = sys.argv.pop(1)


class RoutedSwap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'snapshots.json'
            subprocess.run([BINARY, '--gtest_filter=CriticalCohort.RoutedInventoryUsesNativeSetupHoldAndFailsClosed'],
                           env=dict(os.environ, NEXTPNR_TEST_SWAP_SNAPSHOTS=str(path)), check=True)
            cls.cases = json.loads(path.read_text())
            subprocess.run([BINARY, '--gtest_filter=CriticalCohort.StaticEndpointCoverageRequiresConstantProof'],
                           env=dict(os.environ, NEXTPNR_TEST_STATIC_ENDPOINTS=str(path)), check=True)
            cls.static = json.loads(path.read_text())

    def test_safe_native_route_delay_improvement(self):
        self.assertTrue(guard.compare(self.cases['baseline'], self.cases['safe'])[0])

    def test_predicted_improvement_does_not_authorize_routed_setup_regression(self):
        # Native fixture asserts placement guard acceptance before overriding routed delays.
        ok, reason = guard.compare(self.cases['baseline'], self.cases['predicted_gain_routed_loss'])
        self.assertFalse(ok)
        self.assertIn('setup regression', reason)

    def test_newly_failing_endpoint(self):
        old = guard.rows(self.cases['baseline'])
        new = guard.rows(self.cases['new_failure'])
        self.assertTrue(any(a['setup'] >= 0 > new[k]['setup'] for k, a in old.items()))
        self.assertFalse(guard.compare(self.cases['baseline'], self.cases['new_failure'])[0])

    def test_native_hold_regression(self):
        ok, reason = guard.compare(self.cases['baseline'], self.cases['hold_regression'])
        self.assertFalse(ok)
        self.assertIn('hold regression', reason)

    def test_unsupported_clock_fails_closed(self):
        ok, reason = guard.compare(self.cases['baseline'], self.cases['missing_clock'])
        self.assertFalse(ok)
        self.assertIn('coverage failure', reason)

    def test_missing_endpoint(self):
        candidate = copy.deepcopy(self.cases['safe'])
        candidate['endpoints'].pop(next(iter(candidate['endpoints'])))
        self.assertIn('coverage changed', guard.compare(self.cases['baseline'], candidate)[1])

    def test_clock_phase_and_classification_changes(self):
        for change in ('phase', 'capture', 'setup_timed', 'hold_related'):
            candidate = copy.deepcopy(self.cases['safe'])
            row = next(iter(candidate['endpoints'].values()))[0]
            if change == 'phase':
                candidate['clocks']['clk'][6] += 100
            elif change == 'capture':
                row['capture'][0] = 'other'
            else:
                row[change] = not row[change]
            self.assertFalse(guard.compare(self.cases['baseline'], candidate)[0], change)

    def test_duplicate_and_nonfinite_rows(self):
        for malformed in ('duplicate', 'nonfinite'):
            candidate = copy.deepcopy(self.cases['safe'])
            rows = next(iter(candidate['endpoints'].values()))
            if malformed == 'duplicate':
                rows.append(copy.deepcopy(rows[0]))
            else:
                rows[0]['setup'] = float('nan')
            self.assertFalse(guard.compare(self.cases['baseline'], candidate)[0])

    def test_static_endpoint_semantics_are_invariant(self):
        old = copy.deepcopy(self.cases['baseline'])
        new = copy.deepcopy(self.cases['safe'])
        key, proof = next(iter(self.static.items()))
        old['static_endpoints'][key] = copy.deepcopy(proof)
        new['static_endpoints'][key] = copy.deepcopy(proof)
        self.assertTrue(guard.compare(old, new)[0])
        for field, value in [('value', 0), ('capture', [['CLK', 'other', 0]]),
                             ('chain', [{'cell': 'different'}]), ('sink', {'cell': 'changed'})]:
            changed = copy.deepcopy(new)
            changed['static_endpoints'][key][field] = value
            self.assertIn('static endpoint semantics', guard.compare(old, changed)[1])
        new['static_endpoints'].clear()
        self.assertFalse(guard.compare(old, new)[0])

    def test_static_proof_cannot_replace_missing_timing(self):
        for change in ('old_schema', 'missing_proofs', 'invalid_proof', 'overlap'):
            new = copy.deepcopy(self.cases['safe'])
            if change == 'old_schema':
                new['schema'] = 1
            elif change == 'missing_proofs':
                del new['static_endpoints']
            else:
                key = next(iter(new['endpoints'])) if change == 'overlap' else 'missing'
                new['static_endpoints'][key] = {'reason': 'same-unsupported-on-both-sides'}
            self.assertFalse(guard.compare(self.cases['baseline'], new)[0])

    def bundle(self, directory, case):
        directory.mkdir()
        receipt = {'returncode': 1}
        report = {'fmax': {'clk': {'constraint': 100, 'achieved': 90}},
                  'timing_summary': {'final_analogue_model': True, 'all_pass': False}}
        (directory / 'diagnostic.rbf').write_bytes(case.encode())
        (directory / 'routed.json').write_text(json.dumps({'placement': case, 'pins': case, 'route': case}))
        guard.write_json(directory / 'timing.json', report)
        guard.write_json(directory / 'endpoints.json', self.cases[case])
        guard.write_json(directory / 'receipt.json', receipt)
        return receipt

    def test_rejection_retains_every_baseline_byte(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            before = self.bundle(output / 'baseline', 'baseline')
            old = {n: (output / 'baseline' / n).read_bytes() for n in guard.ARTIFACTS}
            decision = guard.guarded_candidate(output, before,
                lambda: self.bundle(output / 'candidate', 'setup_regression'))
            self.assertEqual(decision['selected'], 'baseline')
            self.assertEqual(decision['retained_returncode'], 1)
            for name, data in old.items():
                self.assertEqual((output / 'retained' / name).read_bytes(), data)
            self.assertTrue((output / 'candidate' / 'diagnostic.rbf').exists())

    def test_exception_restores_baseline_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            before = self.bundle(output / 'baseline', 'baseline')
            def throwing():
                raise RuntimeError('injected route exception')
            decision = guard.guarded_candidate(output, before, throwing)
            self.assertEqual(decision['selected'], 'baseline')
            self.assertIn('injected route exception', decision['reason'])
            self.assertEqual((output / 'retained').resolve(), output / 'baseline')

    def test_safe_candidate_selected_as_one_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            before = self.bundle(output / 'baseline', 'baseline')
            decision = guard.guarded_candidate(output, before, lambda: self.bundle(output / 'candidate', 'safe'))
            self.assertEqual(decision['selected'], 'candidate')
            self.assertTrue(decision['accepted'])
            for name in guard.ARTIFACTS:
                self.assertEqual((output / 'retained' / name).read_bytes(), (output / 'candidate' / name).read_bytes())
            self.assertEqual((output / 'baseline' / 'diagnostic.rbf').read_bytes(), b'baseline')


if __name__ == '__main__':
    unittest.main()
