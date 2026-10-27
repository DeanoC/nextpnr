#!/usr/bin/env python3
"""Reject unsafe or ambiguous placement experiments before packing/routing."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


class Cli(unittest.TestCase):
    def run_design(self, extra=(), settings=None, step=None, plan=None, propose=False, record=True):
        with tempfile.TemporaryDirectory(prefix="routed-placement-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, settings=settings or {}, ports={}, cells={}, netnames={})
            if step is not None:
                module["attributes"]["step"] = step
            source = root / "design.json"
            source.write_text(json.dumps(dict(modules=dict(top=module))))
            options = ["--device", "5CSEBA6U23I7", "--json", str(source), "--router", "gpu", "--placer", "heap"]
            if record:
                options += ["--routed-placement-record", str(root / "native")]
            if propose:
                model = root / "model.json"
                model.write_text("{}")
                options += ["--no-route", "--routed-placement-propose", str(model)]
            else:
                options += ["--rbf", str(root / "core.rbf"), "--report", str(root / "timing.json"),
                            "--write", str(root / "routed.json")]
            if plan is not None:
                path = root / "plan.json"
                path.write_text(plan)
                options += ["--routed-placement-plan", str(path)]
            result = subprocess.run([BINARY, *options, *extra], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True)
            self.assertFalse((root / "native/completion.json").exists())
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stdout

    def test_orphan_control(self):
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-record-calibration"], record=False))
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-trials", "1"], record=False))
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-preserve-routes"], record=False))
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-group-moves"], record=False))
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-check-plan"], record=False))
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-preserve-branches"], record=False))
        self.assertIn("require --routed-placement-record", self.run_design(
            ["--routed-placement-retarget-luts"], record=False))

    def test_lut_retargeting_requires_an_explicit_group_proposal(self):
        self.assertIn("LUT retargeting requires", self.run_design(["--routed-placement-retarget-luts"]))
        self.assertIn("LUT retargeting requires", self.run_design(
            ["--routed-placement-retarget-luts", "--routed-placement-group-moves"], propose=True))

    def test_branch_preservation_requires_the_explicit_group_route_lane(self):
        self.assertIn("Branch preservation requires", self.run_design(["--routed-placement-preserve-branches"]))
        self.assertIn("Branch preservation requires", self.run_design(
            ["--routed-placement-preserve-branches", "--routed-placement-preserve-routes"]))

    def test_calibration_recording_requires_final_routing(self):
        self.assertIn("Calibration recording requires a fresh full recorded route", self.run_design(
            ["--routed-placement-record-calibration"], propose=True))
        self.assertIn("Calibration recording requires a fresh full recorded route", self.run_design(
            ["--routed-placement-record-calibration", "--no-route"]))

    def test_plan_checking_cannot_produce_a_timing_artifact(self):
        self.assertIn("Plan checking requires", self.run_design(["--routed-placement-check-plan"]))
        self.assertIn("Plan checking requires", self.run_design(
            ["--routed-placement-check-plan", "--routed-placement-group-moves", "--no-route"], plan="{}"))

    def test_group_moves_require_fresh_ordinary_placement(self):
        self.assertIn("requires fresh ordinary", self.run_design(["--routed-placement-group-moves", "--no-place"]))

    def test_route_preservation_requires_an_explicit_full_candidate(self):
        self.assertIn("routes require", self.run_design(["--routed-placement-routes", "unused.json"]))
        self.assertIn("routes require", self.run_design(
            ["--routed-placement-preserve-routes", "--routed-placement-routes", "unused.json"], propose=True))

    def test_loaded_checkpoint(self):
        self.assertIn("requires fresh ordinary", self.run_design(step="place"))

    def test_timing_overrides_cannot_authorize_an_experiment(self):
        for key in ("timing/allowFail", "timing/ignoreLoops", "timing/ignoreRelClk"):
            with self.subTest(key=key):
                self.assertIn("rejects saved timing override", self.run_design(settings={key: 1}))
        self.assertIn("rejects force", self.run_design(["--force"]))

    def test_effective_flow_settings(self):
        self.assertIn("requires fresh ordinary", self.run_design(settings={"timing_driven": 0}))
        self.assertIn("requires fresh ordinary", self.run_design(["--critical-cohort-budget", "1"]))

    def test_limits_are_explicit_and_bounded(self):
        self.assertIn("require explicit trial", self.run_design(propose=True))
        for trials, candidates in ((0, 1), (65, 1), (1, 0), (1, 5)):
            with self.subTest(trials=trials, candidates=candidates):
                self.assertIn("limits must be", self.run_design(
                    ["--routed-placement-trials", str(trials), "--routed-placement-candidates", str(candidates)],
                    propose=True))

    def test_invalid_and_duplicate_json_are_rejected_before_scanning_or_placement(self):
        self.assertIn("Invalid routed placement JSON", self.run_design(plan="}"))
        self.assertIn("Duplicate routed placement", self.run_design(plan='{"version": 1, "version": 2}'))


if __name__ == "__main__":
    unittest.main()
