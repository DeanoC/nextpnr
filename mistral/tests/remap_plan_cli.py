#!/usr/bin/env python3
"""Verify staged-remap requests through the real CLI and JSON loader."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


def step(**overrides):
    value = dict(report="guidance/timing.json", candidate=0, groups=1,
                 optimize_pins=False, preserve_ff_placement=True)
    value.update(overrides)
    return value


class RemapPlanCliTest(unittest.TestCase):
    def run_plan(self, plan=None, *, raw=None, options=(), route=False, placer=None,
                 loaded_step=None, reports=None):
        with tempfile.TemporaryDirectory(prefix="remap-plan-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if placer is not None:
                module["settings"] = {"placer": placer}
            if loaded_step is not None:
                module["attributes"]["step"] = loaded_step
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            (root / "plans/guidance").mkdir(parents=True)
            (root / "plans/guidance/timing.json").write_text('{"critical_paths": []}')
            for name, content in (reports or {}).items():
                (root / "plans" / name).write_text(content)
            (root / "plans/plan.json").write_text(raw if raw is not None else json.dumps(plan))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--remap-plan", str(root / "plans/plan.json"),
                       "--write", str(root / "output.json"), "--report", str(root / "output-report.json")]
            if not route:
                command += ["--no-route"]
            # Relative report paths resolve against the plan, not this cwd.
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command + list(options), cwd=root, env=env, timeout=30,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            return result.returncode, result.stdout, any((root / name).exists() for name in
                                                        ("output.json", "output-report.json", "output.rbf"))

    def reject(self, plan, text, **kwargs):
        code, log, output = self.run_plan(plan, **kwargs)
        self.assertNotEqual(code, 0, log)
        self.assertIn(text, log)
        self.assertFalse(output, log)
        self.assertNotIn("Routing...", log)
        return log

    def test_relative_report_and_final_listing(self):
        code, log, output = self.run_plan({"steps": [step(candidate=-1)]})
        self.assertEqual(code, 0, log)
        self.assertIn("Local-remap plan step 0", log)
        self.assertIn("Local remap: 0 qualified candidates", log)
        self.assertTrue(output)

    def test_selected_failure_stops_even_with_force(self):
        log = self.reject({"steps": [step()]}, "did not qualify; routing was not started",
                          options=("--force",), route=True)
        self.assertIn("Local-remap plan step 0", log)

    def test_complete_plan_preloaded_before_placement(self):
        for report, expected in (("missing.json", "local-remap plan report"),
                                 ("invalid.json", "Invalid local-remap plan report")):
            with self.subTest(report=report):
                log = self.reject({"steps": [step(), step(report=report)]}, expected,
                                  reports={"invalid.json": '{"critical_paths": 3}'})
                self.assertNotIn("Local-remap plan step", log)
                self.assertNotIn("Running analytical placer", log)

    def test_strict_schema_and_step_bounds(self):
        for plan in ({"steps": []}, {"steps": [step()] * 9},
                     {"steps": [step()], "unknown": 1},
                     {"steps": [{key: value for key, value in step().items() if key != "groups"}]},
                     {"steps": [step(unknown=True)]}):
            with self.subTest(plan=plan):
                self.reject(plan, "local-remap plan")

    def test_numeric_values_checked_before_cast(self):
        for key, value in (("candidate", -2), ("candidate", 2**31),
                           ("candidate", 0.5), ("candidate", True),
                           ("groups", 0), ("groups", 9), ("groups", "1")):
            with self.subTest(key=key, value=value):
                self.reject({"steps": [step(**{key: value})]}, "Invalid integer")

    def test_paths_and_booleans_are_typed(self):
        for key, value in (("report", ""), ("report", "a\x00b"),
                           ("optimize_pins", 1), ("preserve_ff_placement", "true")):
            with self.subTest(key=key, value=value):
                self.reject({"steps": [step(**{key: value})]}, "path or boolean")

    def test_duplicate_escaped_key_is_rejected(self):
        raw = json.dumps({"steps": [step()]}).replace('"candidate": 0',
                '"candidate": 0, "candi\\u0064ate": 1')
        self.reject(None, "Duplicate local-remap plan key", raw=raw)

    def test_listing_must_be_final_and_cannot_route_or_write_rbf(self):
        self.reject({"steps": [step(candidate=-1), step()]}, "listing step must be last")
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last", route=True)
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last",
                    options=("--rbf", "output.rbf"))

    def test_final_listing_cannot_precede_legacy_comb_remapping(self):
        report = ("--remap-comb-critical", "plans/guidance/timing.json")
        for options in (report, report + ("--remap-comb-candidate", "-1"),
                        report + ("--remap-comb-candidate", "0"),
                        ("--remap-comb-candidate", "0")):
            with self.subTest(options=options):
                log = self.reject({"steps": [step(candidate=-1)]},
                                  "Local-remap listing must be final", options=options)
                self.assertNotIn("Running analytical placer", log)
                self.assertNotIn("Local-remap plan step", log)
                self.assertNotIn("Comb remap:", log)

    def test_selected_local_plan_can_precede_legacy_comb_remapping(self):
        log = self.reject({"steps": [step()]}, "did not qualify; routing was not started",
                          options=("--remap-comb-critical", "plans/guidance/timing.json",
                                   "--remap-comb-candidate", "0"))
        self.assertIn("Local-remap plan step 0", log)
        self.assertNotIn("Comb remap:", log)
        self.assertNotIn("cannot precede", log)

    def test_requires_fresh_full_heap_placement(self):
        for option in ("--no-pack", "--no-place", "--pack-only", "--fes-scaffold"):
            with self.subTest(option=option):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement", options=(option,))
        self.reject({"steps": [step()]}, "fresh ordinary HeAP placement", loaded_step="place")

    def test_effective_json_sa_rejected_even_with_cli_heap(self):
        self.reject({"steps": [step(candidate=-1)]}, "ordinary full-design HeAP placement",
                    placer="sa", options=("--placer", "heap"))

    def test_legacy_options_cannot_mix_with_plan(self):
        for options in (("--remap-candidate", "0"), ("--remap-groups", "1"),
                        ("--remap-optimize-pins",), ("--remap-preserve-ffs",)):
            with self.subTest(options=options):
                self.reject({"steps": [step()]}, "cannot be combined with legacy", options=options)


if __name__ == "__main__":
    unittest.main()
