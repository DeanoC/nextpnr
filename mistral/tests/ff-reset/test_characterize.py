#!/usr/bin/env python3
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

import characterize


class CharacterizerTest(unittest.TestCase):
    def test_failed_fitted_checks_do_not_write_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'output'
            with patch.object(sys, 'argv', ['characterize.py', '--quartus-bin', str(root / 'quartus'),
                                            '--output', str(output)]), \
                    patch.object(characterize.subprocess, 'check_output',
                                 return_value='Version 17.0.2 Build 602\n'), \
                    patch.object(characterize.subprocess, 'run') as run, \
                    patch.object(characterize, 'summarize',
                                 return_value={'all_fitted_checks_pass': False}) as summarize:
                with self.assertRaises(SystemExit) as failure:
                    characterize.main()

            self.assertEqual(failure.exception.code,
                             'rates: fitted timing checks failed; refusing to write reference')
            self.assertEqual(run.call_count, 2)
            summarize.assert_called_once()
            self.assertEqual(list(output.rglob('reference.json')), [])

    def test_model_limit_failure_publishes_no_project_or_top_level_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'output'

            def summarize(project):
                return {'all_fitted_checks_pass': True,
                        'observed_max_ns': {'recovery': .171 if project.name == 'rates' else .164,
                                            'removal': .075}}

            with patch.object(sys, 'argv', ['characterize.py', '--quartus-bin', str(root / 'quartus'),
                                            '--output', str(output)]), \
                    patch.object(characterize.subprocess, 'check_output',
                                 return_value='Version 17.0.2 Build 602\n'), \
                    patch.object(characterize.subprocess, 'run') as run, \
                    patch.object(characterize, 'summarize', side_effect=summarize):
                with self.assertRaisesRegex(SystemExit, 'exceeds model limits'):
                    characterize.main()

            self.assertEqual(run.call_count, 6)
            self.assertFalse(output.exists())
            self.assertEqual(list(root.rglob('reference.json')), [])
            self.assertEqual(list(root.glob('.output.*')), [])


if __name__ == '__main__':
    unittest.main()
