#!/usr/bin/env python3
"""Validate explicit opt-in and unsupported placement modes before routing."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


class CriticalCohortCli(unittest.TestCase):
    def run_design(self, options=(), step=None, settings=None, report=None, route=False):
        with tempfile.TemporaryDirectory(prefix="critical-cohort-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if step is not None:
                module["attributes"]["step"] = step
            if settings is not None:
                module["settings"] = settings
            source = root / "design.json"
            source.write_text(json.dumps({"modules": {"top": module}}))
            if report is not None:
                guidance = root / "report.json"
                guidance.write_text(json.dumps(report))
                options = [*options, "--critical-cohort-report", str(guidance)]
            return subprocess.run([BINARY, "--device", "5CSEBA6U23I7", "--json", str(source),
                                   *([] if route else ["--no-route"]), *options], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=60)

    def test_model_export_requires_full_bitstream_route(self):
        result = self.run_design(["--critical-cohort-model-out", "/tmp/unused-cohort-model.json"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires a fresh ordinary full route with --rbf", result.stdout)

    def test_model_export_rejects_positive_repair_budget(self):
        with tempfile.TemporaryDirectory(prefix="cohort-model-export-") as directory:
            model = Path(directory) / "model.json"
            bitstream = Path(directory) / "design.rbf"
            for budget in (1, 64):
                with self.subTest(budget=budget):
                    result = self.run_design(["--critical-cohort-model-out", str(model),
                                              "--rbf", str(bitstream),
                                              "--critical-cohort-budget", str(budget)], route=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("requires --critical-cohort-budget 0", result.stdout)
                    self.assertFalse(model.exists())
                    self.assertFalse(bitstream.exists())

    def test_model_export_accepts_baseline_budget(self):
        with tempfile.TemporaryDirectory(prefix="cohort-model-export-") as directory:
            for budget in (None, 0):
                with self.subTest(budget=budget):
                    model = Path(directory) / f"model-{budget}.json"
                    bitstream = Path(directory) / f"design-{budget}.rbf"
                    options = ["--critical-cohort-model-out", str(model), "--rbf", str(bitstream)]
                    if budget is not None:
                        options += ["--critical-cohort-budget", str(budget)]
                    result = self.run_design(options, route=True)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    self.assertTrue(bitstream.exists())
                    report = json.loads(model.read_text())
                    self.assertTrue(report["timing_summary"]["final_analogue_model"])
                    self.assertIn("cohort_route_model", report)

    def test_invalid_budget(self):
        for budget in (-1, 65):
            with self.subTest(budget=budget):
                result = self.run_design(["--critical-cohort-budget", str(budget)])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("must be between 0 and 64", result.stdout)

    def test_report_requires_positive_budget(self):
        result = self.run_design(report={})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires a positive --critical-cohort-budget", result.stdout)

    def test_report_requires_final_analogue_timing(self):
        result = self.run_design(["--critical-cohort-budget", "1"], report={"critical_paths": []})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires a final analogue timing report", result.stdout)

    def test_unsupported_modes(self):
        for options in (["--no-pack"], ["--no-place"], ["--pack-only"], ["--placer", "sa"]):
            with self.subTest(options=options):
                result = self.run_design(["--critical-cohort-budget", "1", *options])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires fresh ordinary HeAP placement", result.stdout)

    def test_loaded_placer_cannot_bypass_mode_check(self):
        result = self.run_design(["--critical-cohort-budget", "1"], settings={"placer": "sa"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires fresh ordinary HeAP placement", result.stdout)

    def test_placed_checkpoint_cannot_enable_repair(self):
        result = self.run_design(["--critical-cohort-budget", "1"], step="place")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires fresh ordinary HeAP placement", result.stdout)

    def test_saved_setting_does_not_enable_repair(self):
        result = self.run_design(settings={"mistral/criticalCohortBudget": "16"})
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("Critical cohort:", result.stdout)


if __name__ == "__main__":
    unittest.main()
