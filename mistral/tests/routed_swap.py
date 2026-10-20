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
            subprocess.run([BINARY, '--gtest_filter=IoDelayTest.ExternalEndpointInventory'],
                           env=dict(os.environ, NEXTPNR_TEST_IO_ENDPOINTS=str(path)), check=True)
            cls.io = json.loads(path.read_text())
            subprocess.run([BINARY, '--gtest_filter=IoDelayTest.ExternalOutputEndpointInventory'],
                           env=dict(os.environ, NEXTPNR_TEST_OUTPUT_ENDPOINTS=str(path)), check=True)
            cls.outputs = json.loads(path.read_text())

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
        key, proof = next((key, proof) for key, proof in self.static.items()
                          if key not in old['endpoints'])
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

    def test_native_external_input_paths(self):
        self.assertTrue(guard.compare(self.io['baseline'], self.io['safe'])[0])
        for case, reason in [('new_failure', 'setup regression'), ('hold_regression', 'hold regression'),
                             ('clock_latency', 'hold regression'), ('missing', 'coverage failure'),
                             ('async_reset', 'coverage failure'), ('external_reset', 'coverage failure'), ('min_changed', 'IO boundary constraints changed'),
                             ('max_changed', 'IO boundary constraints changed'), ('phase', 'clock constraints changed'),
                             ('falling', 'clock constraints changed')]:
            ok, message = guard.compare(self.io['baseline'], self.io[case])
            self.assertFalse(ok, case)
            self.assertIn(reason, message, case)

    def test_external_contract_changes_cannot_purchase_gain(self):
        for field, value in [('min', 0.5), ('max', 2), ('clock', 'missing'),
                             ('fall', True), ('port', 'other'), ('input', False)]:
            changed = copy.deepcopy(self.io['safe'])
            changed['io_constraints'][0][field] = value
            self.assertFalse(guard.compare(self.io['baseline'], changed)[0], field)
        for mode in ('missing', 'duplicate', 'nonfinite', 'reversed'):
            changed = copy.deepcopy(self.io['safe'])
            if mode == 'missing':
                del changed['io_constraints']
            elif mode == 'duplicate':
                changed['io_constraints'] *= 2
            elif mode == 'nonfinite':
                changed['io_constraints'][0]['min'] = float('nan')
            else:
                changed['io_constraints'][0]['min'] = 99
            self.assertFalse(guard.compare(self.io['baseline'], changed)[0], mode)

    def test_both_sides_require_explicit_boundary_metadata(self):
        old, new = copy.deepcopy(self.io['baseline']), copy.deepcopy(self.io['safe'])
        del old['io_constraints']
        del new['io_constraints']
        self.assertFalse(guard.compare(old, new)[0])
        old, new = copy.deepcopy(self.cases['baseline']), copy.deepcopy(self.cases['safe'])
        del old['io_constraints']
        del new['io_constraints']
        self.assertFalse(guard.compare(old, new)[0])

    def test_external_failures_retain_exact_baseline(self):
        previous = self.cases
        self.cases = self.io
        try:
            for case in ('new_failure', 'hold_regression', 'missing', 'async_reset', 'external_reset', 'min_changed', 'max_changed'):
                with tempfile.TemporaryDirectory() as tmp:
                    output = Path(tmp)
                    before = self.bundle(output / 'baseline', 'baseline')
                    original = {n: (output / 'baseline' / n).read_bytes() for n in guard.ARTIFACTS}
                    decision = guard.guarded_candidate(output, before,
                        lambda: self.bundle(output / 'candidate', case))
                    self.assertEqual(decision['selected'], 'baseline', case)
                    for name, data in original.items():
                        self.assertEqual((output / 'retained' / name).read_bytes(), data, case)
        finally:
            self.cases = previous

    def test_native_output_coverage_and_retention(self):
        self.assertTrue(guard.compare(self.outputs['baseline'], self.outputs['safe'])[0])
        previous = self.cases
        self.cases = self.outputs
        try:
            for case in ('new_failure', 'hold_regression', 'clock_latency', 'min_changed',
                         'max_changed', 'missing', 'phase', 'falling_capture', 'falling_launch', 'constant'):
                self.assertFalse(guard.compare(self.outputs['baseline'], self.outputs[case])[0], case)
                with tempfile.TemporaryDirectory() as tmp:
                    output = Path(tmp)
                    before = self.bundle(output / 'baseline', 'baseline')
                    original = {n: (output / 'baseline' / n).read_bytes() for n in guard.ARTIFACTS}
                    decision = guard.guarded_candidate(output, before,
                        lambda: self.bundle(output / 'candidate', case))
                    self.assertEqual(decision['selected'], 'baseline', case)
                    for name, data in original.items():
                        self.assertEqual((output / 'retained' / name).read_bytes(), data, case)
        finally:
            self.cases = previous

    def test_output_contract_is_complete_and_invariant(self):
        for field, value in [('min', 0.5), ('max', 2), ('clock', 'missing'),
                             ('fall', True), ('port', 'other'), ('input', True)]:
            changed = copy.deepcopy(self.outputs['safe'])
            row = next(c for c in changed['io_constraints'] if not c['input'])
            row[field] = value
            self.assertFalse(guard.compare(self.outputs['baseline'], changed)[0], field)
        for mode in ('missing', 'duplicate', 'omit_output', 'omit_contracts', 'both_omit',
                     'polarity', 'oe_net', 'direction', 'boolean_direction', 'boolean_pin', 'remove_oe'):
            old, new = copy.deepcopy(self.outputs['baseline']), copy.deepcopy(self.outputs['safe'])
            if mode == 'missing':
                del new['io_constraints']
            elif mode == 'duplicate':
                new['io_constraints'] *= 2
            elif mode == 'omit_output':
                new['output_contracts'].pop('dout')
            elif mode in ('omit_contracts', 'both_omit'):
                del new['output_contracts']
                if mode == 'both_omit':
                    del old['output_contracts']
            elif mode in ('direction', 'boolean_direction'):
                new['output_contracts']['dout']['direction'] = 999 if mode == 'direction' else True
            elif mode == 'boolean_pin':
                old['output_contracts']['dq[0]']['cells']['dq_pad']['pins']['OE'] = 0
                new['output_contracts']['dq[0]']['cells']['dq_pad']['pins']['OE'] = False
            elif mode == 'remove_oe':
                new['endpoints'].pop('["dq_pad", "OE"]')
            else:
                cell = new['output_contracts']['dq[0]']['cells']['dq_pad']
                if mode == 'polarity':
                    cell['pins']['OE'] = 999
                else:
                    cell['ports']['OE'][1] = 'different_net'
            self.assertFalse(guard.compare(old, new)[0], mode)

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
