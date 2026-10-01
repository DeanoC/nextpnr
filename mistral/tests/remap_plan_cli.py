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
                 loaded_step=None, reports=None, post=False, early_plan=None, settings=None):
        with tempfile.TemporaryDirectory(prefix="remap-plan-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if settings is not None:
                module["settings"] = dict(settings)
            if placer is not None:
                module.setdefault("settings", {})["placer"] = placer
            if loaded_step is not None:
                module["attributes"]["step"] = loaded_step
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            (root / "plans/guidance").mkdir(parents=True)
            (root / "plans/guidance/timing.json").write_text('{"critical_paths": []}')
            for name, content in (reports or {}).items():
                (root / "plans" / name).write_text(content)
            (root / "plans/plan.json").write_text(raw if raw is not None else json.dumps(plan))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--remap-post-plan" if post else "--remap-plan", str(root / "plans/plan.json"),
                       "--write", str(root / "output.json"), "--report", str(root / "output-report.json")]
            if early_plan is not None:
                self.assertTrue(post, "an additional early plan requires a post-plan fixture")
                (root / "plans/early.json").write_text(json.dumps(early_plan))
                command += ["--remap-plan", str(root / "plans/early.json")]
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
        for plan in ({"steps": []}, {"steps": [step()] * 17},
                     {"steps": [step()], "unknown": 1},
                     {"steps": [{key: value for key, value in step().items() if key != "groups"}]},
                     {"steps": [step(unknown=True)]}):
            with self.subTest(plan=plan):
                self.reject(plan, "local-remap plan")

    def test_longer_bounded_plans_reach_selection(self):
        for count in (9, 16):
            with self.subTest(count=count):
                log = self.reject({"steps": [step()] * count},
                                  "did not qualify; routing was not started",
                                  options=("--force",), route=True)
                self.assertIn("Local-remap plan step 0:", log)

    def test_sixteenth_report_preloaded_before_placement(self):
        for report, expected in (("missing.json", "local-remap plan report"),
                                 ("invalid.json", "Invalid local-remap plan report")):
            with self.subTest(report=report):
                plan = {"steps": [step()] * 15 + [step(report=report)]}
                log = self.reject(plan, expected, reports={"invalid.json": '{"critical_paths": 3}'})
                self.assertNotIn("Local-remap plan step", log)
                self.assertNotIn("Running analytical placer", log)

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

    def test_post_relative_report_and_final_listing(self):
        code, log, output = self.run_plan({"steps": [step(candidate=-1)]}, post=True)
        self.assertEqual(code, 0, log)
        self.assertIn("Local-remap post-plan step 0", log)
        self.assertNotIn("Local-remap plan step", log)
        self.assertIn("Local remap: 0 qualified candidates", log)
        self.assertTrue(output)

    def test_post_selected_failure_stops_even_with_force(self):
        log = self.reject({"steps": [step()]}, "did not qualify; routing was not started",
                          post=True, options=("--force",), route=True)
        self.assertIn("Local-remap post-plan step 0", log)
        self.assertNotIn("Local-remap plan step", log)

    def test_post_strict_schema_values_and_duplicate_keys(self):
        invalid = (({"steps": []}, "Invalid post-remap plan"),
                   ({"steps": [step()] * 17}, "Invalid post-remap plan"),
                   ({"steps": [step()], "unknown": 1}, "Invalid post-remap plan"),
                   ({"steps": [step(unknown=True)]}, "plan step fields"),
                   ({"steps": [{key: value for key, value in step().items() if key != "groups"}]}, "plan step fields"),
                   ({"steps": [step(candidate=2**31)]}, "Invalid integer"),
                   ({"steps": [step(candidate=True)]}, "Invalid integer"),
                   ({"steps": [step(groups=0)]}, "Invalid integer"),
                   ({"steps": [step(groups=9)]}, "Invalid integer"),
                   ({"steps": [step(groups="1")]}, "Invalid integer"),
                   ({"steps": [step(report="a\x00b")]}, "path or boolean"),
                   ({"steps": [step(optimize_pins=1)]}, "path or boolean"))
        for plan, expected in invalid:
            with self.subTest(plan=plan):
                log = self.reject(plan, expected, post=True)
                self.assertNotIn("Running analytical placer", log)
        raw = json.dumps({"steps": [step()]}).replace('"candidate": 0',
                '"candidate": 0, "candi\\u0064ate": 1')
        self.reject(None, "Duplicate post-remap plan key", raw=raw, post=True)

    def test_post_reports_preloaded_before_any_placement(self):
        for report, expected in (("missing.json", "post-remap plan report"),
                                 ("invalid.json", "Invalid post-remap plan report")):
            with self.subTest(report=report):
                log = self.reject({"steps": [step(), step(report=report)]}, expected, post=True,
                                  early_plan={"steps": [step()]},
                                  reports={"invalid.json": '{"critical_paths": 3}'})
                self.assertNotIn("Local-remap plan step", log)
                self.assertNotIn("Local-remap post-plan step", log)
                self.assertNotIn("Running analytical placer", log)

    def test_post_combined_step_limit_and_last_report_preload(self):
        log = self.reject({"steps": [step()] * 8}, "did not qualify; routing was not started",
                          post=True, early_plan={"steps": [step()] * 8},
                          options=("--force",), route=True)
        self.assertIn("Local-remap plan step 0:", log)
        self.assertNotIn("Local-remap post-plan step", log)
        log = self.reject({"steps": [step()] * 8}, "Combined early and post-remap plans exceed 16",
                          post=True, early_plan={"steps": [step()] * 9})
        self.assertNotIn("Running analytical placer", log)
        self.assertNotIn("Local-remap plan step", log)
        log = self.reject({"steps": [step()] * 7 + [step(report="invalid.json")]},
                          "Invalid post-remap plan report", post=True,
                          early_plan={"steps": [step()] * 8},
                          reports={"invalid.json": '{"critical_paths": 3}'})
        self.assertNotIn("Running analytical placer", log)
        self.assertNotIn("Local-remap plan step", log)

    def test_post_listing_must_be_final_and_cannot_route_or_write_rbf(self):
        self.reject({"steps": [step(candidate=-1), step()]}, "listing step must be last", post=True)
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last", post=True, route=True)
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last", post=True,
                    options=("--rbf", "output.rbf"))

    def test_post_requires_fresh_full_heap_placement(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--fes-scaffold",), ("--fes-cart", "missing.json"), ("--placer", "sa")):
            with self.subTest(options=options):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement", post=True, options=options)
        for loaded in ("pack", "place", "route"):
            with self.subTest(loaded=loaded):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement", post=True,
                            loaded_step=loaded, options=("--force",))

    def test_post_effective_json_sa_rejected_even_with_cli_heap(self):
        self.reject({"steps": [step(candidate=-1)]}, "ordinary full-design HeAP placement",
                    post=True, placer="sa", options=("--placer", "heap"))

    def test_post_cannot_follow_any_prior_listing(self):
        cases = ((None, ("--remap-critical", "plans/guidance/timing.json"), {}),
                 ({"steps": [step(candidate=-1)]}, (), {}),
                 (None, ("--remap-comb-critical", "plans/guidance/timing.json"), {}),
                 (None, ("--remap-comb-plan", "plans/comb.json"),
                  {"comb.json": json.dumps({"steps": [{"report": "guidance/timing.json", "candidate": -1}]})}),
                 (None, ("--remap-decompose-critical", "plans/guidance/timing.json"), {}))
        for early, options, reports in cases:
            with self.subTest(options=options, early=early):
                log = self.reject({"steps": [step(candidate=-1)]}, "listing", post=True,
                                  early_plan=early, options=options, reports=reports)
                self.assertNotIn("Running analytical placer", log)
                self.assertNotIn("Local-remap post-plan step", log)

    def test_post_listing_excludes_any_following_driver_copy(self):
        for candidate in (None, -1, 0):
            options = ("--remap-lut-driver-critical", "plans/guidance/timing.json")
            if candidate is not None:
                options += ("--remap-lut-driver-candidate", str(candidate))
            with self.subTest(candidate=candidate):
                log = self.reject({"steps": [step(candidate=-1)]}, "listing", post=True, options=options)
                self.assertNotIn("Running analytical placer", log)
                self.assertNotIn("Local-remap post-plan step", log)

    def test_json_post_plan_settings_do_not_enable_a_pass(self):
        code, log, output = self.run_plan({"steps": [step(candidate=-1)]},
                                        settings={"mistral/remapPostPlan": "missing.json",
                                                  "mistral/remapPostPlanListOnly": 1})
        self.assertEqual(code, 0, log)
        self.assertIn("Local-remap plan step 0", log)
        self.assertNotIn("Local-remap post-plan step", log)
        self.assertTrue(output)


if __name__ == "__main__":
    unittest.main()
