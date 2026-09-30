#!/usr/bin/env python3
"""Exercise local-remap mode guards through the real JSON/CLI loading path."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class LocalRemapCliTest(unittest.TestCase):
    def run_design(self, placer=None, options=(), remap=True, comb=False):
        with tempfile.TemporaryDirectory(prefix="local-remap-cli-") as directory:
            root = Path(directory)
            module = {"attributes": {"top": 1}, "ports": {}, "cells": {}, "netnames": {}}
            if placer is not None:
                module["settings"] = {"placer": placer}
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            (root / "timing.json").write_text('{"critical_paths": []}')
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--no-route", "--write", str(root / "output.json")]
            if comb:
                command += ["--remap-comb-critical", str(root / "timing.json")]
            elif remap:
                command += ["--remap-critical", str(root / "timing.json")]
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command + list(options), stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, universal_newlines=True, env=env, timeout=30)
            return result.returncode, result.stdout, (root / "output.json").exists()

    def test_rejects_effective_sa_from_json_even_with_cli_heap(self):
        for options in ((), ("--placer", "heap")):
            with self.subTest(options=options):
                code, log, output = self.run_design("sa", options)
                self.assertNotEqual(code, 0, log)
                self.assertIn("Local remap requires ordinary full-design HeAP placement", log)
                self.assertNotIn("Running simulated annealing placer.", log)
                self.assertNotIn("Local remap: 0 qualified candidates", log)
                self.assertFalse(output)

    def test_rejects_pack_only_with_list_or_candidate(self):
        for selection in ((), ("--remap-candidate", "0")):
            with self.subTest(selection=selection):
                code, log, output = self.run_design(options=("--pack-only",) + selection)
                self.assertNotEqual(code, 0, log)
                self.assertIn("Local remap requires fresh ordinary HeAP placement", log)
                self.assertFalse(output)

    def test_heap_list_mode_still_runs(self):
        code, log, output = self.run_design("heap")
        self.assertEqual(code, 0, log)
        self.assertIn("Local remap: 0 qualified candidates", log)
        self.assertTrue(output)

    def test_pin_optimization_requires_remapping(self):
        code, log, output = self.run_design(options=("--remap-optimize-pins",), remap=False)
        self.assertNotEqual(code, 0, log)
        self.assertIn("Local-remap options require --remap-critical", log)
        self.assertFalse(output)

    def test_pin_optimization_preserves_list_mode(self):
        code, log, output = self.run_design("heap", options=("--remap-optimize-pins",))
        self.assertEqual(code, 0, log)
        self.assertIn("Local remap: 0 qualified candidates", log)
        self.assertTrue(output)

    def test_pin_optimization_keeps_placement_mode_guard(self):
        code, log, output = self.run_design(options=("--remap-optimize-pins", "--no-place"))
        self.assertNotEqual(code, 0, log)
        self.assertIn("Local remap requires fresh ordinary HeAP placement", log)
        self.assertFalse(output)

    def test_pack_only_without_remapping_still_runs(self):
        code, log, output = self.run_design(options=("--pack-only",), remap=False)
        self.assertEqual(code, 0, log)
        self.assertTrue(output)

    def test_comb_candidate_requires_report(self):
        code, log, output = self.run_design(options=("--remap-comb-candidate", "0"), remap=False)
        self.assertNotEqual(code, 0, log)
        self.assertIn("--remap-comb-candidate requires --remap-comb-critical", log)
        self.assertFalse(output)

    def test_comb_list_with_effective_heap_runs(self):
        code, log, output = self.run_design("heap", comb=True)
        self.assertEqual(code, 0, log)
        self.assertIn("Comb remap: 0 qualified candidates", log)
        self.assertTrue(output)

    def test_comb_rejects_effective_sa_even_with_cli_heap(self):
        for options in ((), ("--placer", "heap")):
            with self.subTest(options=options):
                code, log, output = self.run_design("sa", options=options, comb=True)
                self.assertNotEqual(code, 0, log)
                self.assertIn("Comb remap requires ordinary full-design HeAP placement", log)
                self.assertFalse(output)

    def test_comb_rejects_skipped_placement(self):
        for option in ("--pack-only", "--no-place"):
            with self.subTest(option=option):
                code, log, output = self.run_design(options=(option,), comb=True)
                self.assertNotEqual(code, 0, log)
                self.assertIn("Comb remap requires fresh ordinary HeAP placement", log)
                self.assertFalse(output)

    def test_comb_rejects_invalid_or_unqualified_selection(self):
        for index, expected in (("-2", "Invalid comb-remap candidate index"),
                                ("0", "Requested comb-remap candidate was not qualified")):
            with self.subTest(index=index):
                code, log, output = self.run_design("heap", options=("--remap-comb-candidate", index), comb=True)
                self.assertNotEqual(code, 0, log)
                self.assertIn(expected, log)
                self.assertFalse(output)


if __name__ == "__main__":
    unittest.main()
