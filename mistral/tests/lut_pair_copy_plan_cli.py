#!/usr/bin/env python3
"""Exercise bounded composed-copy plans through the real CLI and JSON loader."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


def step(**overrides):
    value = dict(report="guidance/timing.json", candidate=0)
    value.update(overrides)
    return value


class LutPairCopyPlanCliTest(unittest.TestCase):
    def run_plan(self, plan=None, *, raw=None, options=(), route=False, enabled=True,
                 loaded_step=None, placer=None, reports=None, settings=None,
                 prefix=None, prefix_candidate=-1, driver=False,
                 environment=None, reload=False, collect=False):
        with tempfile.TemporaryDirectory(prefix="lut-pair-copy-plan-cli-") as directory:
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
                path = root / "plans" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                if content is None:
                    if path.exists():
                        path.unlink()
                else:
                    path.write_text(content)
            (root / "plans/plan.json").write_text(raw if raw is not None else json.dumps(plan))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--write", str(root / "output.json")]
            if enabled:
                command += ["--remap-lut-pair-copy-plan", str(root / "plans/plan.json")]
            if not route:
                command += ["--no-route"]
            if prefix in ("local", "comb", "decomposition"):
                critical, candidate = {
                    "local": ("--remap-critical", "--remap-candidate"),
                    "comb": ("--remap-comb-critical", "--remap-comb-candidate"),
                    "decomposition": ("--remap-decompose-critical", "--remap-decompose-candidate"),
                }[prefix]
                command += [critical, "plans/guidance/timing.json", candidate, str(prefix_candidate)]
            elif prefix in ("local-plan", "post-plan", "comb-plan"):
                earlier = dict(report="guidance/timing.json", candidate=prefix_candidate)
                if prefix != "comb-plan":
                    earlier.update(groups=1, optimize_pins=False, preserve_ff_placement=True)
                (root / "plans/prefix.json").write_text(json.dumps({"steps": [earlier]}))
                option = {"local-plan": "--remap-plan", "post-plan": "--remap-post-plan",
                          "comb-plan": "--remap-comb-plan"}[prefix]
                command += [option, "plans/prefix.json"]
            if driver:
                command += ["--remap-lut-driver-critical", "plans/guidance/timing.json",
                            "--remap-lut-driver-candidate", "0"]
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            env.update(environment or {})
            result = subprocess.run(command + list(options), cwd=root, env=env, timeout=45,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            outputs = {name for name in ("output.json", "output.rbf") if (root / name).exists()}
            answer = result.returncode, result.stdout, outputs
            if reload and result.returncode == 0:
                again = subprocess.run(
                    [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "output.json"),
                     "--no-pack", "--no-place", "--no-route", "--write", str(root / "reloaded.json")],
                    cwd=root, env=env, timeout=45, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True)
                return answer + (again.returncode, again.stdout, (root / "reloaded.json").exists())
            if collect:
                documents = {name: json.loads((root / name).read_text())
                             for name in ("output.json",) if (root / name).exists()}
                return answer + (documents,)
            return answer

    def reject(self, plan, reason, *, before_placement=False, **kwargs):
        code, log, outputs = self.run_plan(plan, **kwargs)
        self.assertNotEqual(code, 0, log)
        self.assertIn(reason, log)
        self.assertFalse(outputs, log)
        self.assertNotIn("Routing...", log)
        self.assertNotIn("Routing complete.", log)
        self.assertNotIn("Running the GPU router", log)
        if before_placement:
            self.assertNotIn("Running analytical placer", log)
            self.assertNotIn("Info: LUT pair copy plan step ", log)
            self.assertNotIn("LUT pair copy discovery", log)
        return log

    def test_relative_report_and_final_listing_write_only_placement(self):
        # The report lives beside the plan, rather than beside the process cwd.
        code, log, outputs = self.run_plan({"steps": [step(candidate=-1)]}, placer="heap")
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"}, log)
        self.assertIn("LUT pair copy plan step 0", log)
        self.assertIn("LUT pair copy discovery: 0 bounded cones.", log)
        self.assertIn("LUT pair copy: 0 qualified candidates; no candidate applied.", log)
        self.assertNotIn("LUT pair placement discovery", log)
        self.assertNotIn("Routing...", log)
        self.assertNotIn("Running the GPU router", log)

    def test_strict_plan_schema_and_two_step_limit(self):
        for plan in (None, [], {"steps": []}, {"steps": [step()] * 3},
                     {"steps": [step()], "unknown": True}, {"steps": "invalid"}):
            with self.subTest(plan=plan):
                self.reject(plan, "Invalid LUT pair copy plan", before_placement=True)
        for raw in ("{", "", "null"):
            with self.subTest(raw=raw):
                self.reject(None, "Invalid LUT pair copy plan", raw=raw, before_placement=True)
        for entry in (None, [], {"report": "guidance/timing.json"}, {"candidate": 0},
                      step(unknown=True), step(groups=1)):
            with self.subTest(entry=entry):
                self.reject({"steps": [entry]}, "Invalid LUT pair copy plan step fields",
                            before_placement=True)

    def test_candidate_checked_before_integer_conversion(self):
        for candidate in (-2, 2**31, 0.5, True, "0", None):
            with self.subTest(candidate=candidate):
                self.reject({"steps": [step(candidate=candidate)]},
                            "Invalid integer in LUT pair copy plan", before_placement=True)
        # json11 may reject an overflowing number while parsing, or leave it
        # for the explicit finite-number guard; both must fail before placement.
        raw = '{"steps": [{"report": "guidance/timing.json", "candidate": 1e309}]}'
        self.reject(None, "LUT pair copy plan", raw=raw, before_placement=True)

    def test_report_path_must_be_nonempty_string_without_nul(self):
        for report in ("", "a\x00b", 1, True, None, []):
            with self.subTest(report=report):
                self.reject({"steps": [step(report=report)]}, "Invalid LUT pair copy plan path",
                            before_placement=True)

    def test_duplicate_and_escaped_plan_keys_are_rejected(self):
        values = (
            '{"steps": [], "steps": [{"report":"guidance/timing.json","candidate":0}]}',
            '{"steps": [{"report":"guidance/timing.json","candidate":0,"candidate":1}]}',
            '{"steps": [{"report":"guidance/timing.json","candidate":0,"candi\\u0064ate":1}]}',
        )
        for raw in values:
            with self.subTest(raw=raw):
                self.reject(None, "Duplicate LUT pair copy plan key", raw=raw, before_placement=True)

    def test_second_report_preloaded_before_any_placement_or_copy(self):
        for report, content in (("missing.json", None), ("invalid.json", "{"),
                                ("invalid.json", "[]"),
                                ("invalid.json", '{"critical_paths": 3}'),
                                ("invalid.json", '{"critical_paths": [], "critical_paths": []}')):
            with self.subTest(report=report, content=content):
                reports = {report: content} if content is not None else {}
                self.reject({"steps": [step(), step(report=report)]}, "LUT pair copy plan report",
                            reports=reports, before_placement=True)

    def test_later_report_path_schema_is_checked_before_placement(self):
        for path in (None, {"path": "invalid"}, {"path": [], "max_delay": "invalid"}):
            with self.subTest(path=path):
                report = json.dumps({"critical_paths": [path]})
                self.reject({"steps": [step(), step(report="invalid.json")]},
                            "LUT pair copy plan report", reports={"invalid.json": report},
                            before_placement=True)

    def test_listing_must_be_last_without_route_or_rbf(self):
        self.reject({"steps": [step(candidate=-1), step()]}, "listing step must be last",
                    before_placement=True)
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last",
                    route=True, before_placement=True)
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last",
                    options=("--rbf", "output.rbf"), before_placement=True)

    def test_final_listing_forbids_every_following_driver_request(self):
        report = ("--remap-lut-driver-critical", "plans/guidance/timing.json")
        for options in (report, report + ("--remap-lut-driver-candidate", "-1"),
                        report + ("--remap-lut-driver-candidate", "0"),
                        ("--remap-lut-driver-candidate", "0")):
            with self.subTest(options=options):
                self.reject({"steps": [step(candidate=-1)]},
                            "A LUT pair copy plan listing must be final; it cannot precede LUT driver copy",
                            options=options, before_placement=True)

    def test_all_legacy_pair_options_conflict_with_plan(self):
        for options in (("--remap-lut-pair-critical", "plans/guidance/timing.json"),
                        ("--remap-lut-pair-candidate", "0"),
                        ("--remap-lut-pair-compose-copy",)):
            with self.subTest(options=options):
                self.reject({"steps": [step()]}, "cannot be combined with legacy LUT pair options",
                            options=options, before_placement=True)

    def test_every_earlier_listing_is_final_before_copy_plan(self):
        for prefix in ("local", "comb", "decomposition", "local-plan", "comb-plan", "post-plan"):
            with self.subTest(prefix=prefix):
                self.reject({"steps": [step(candidate=-1)]},
                            "A remap listing must be final; it cannot precede LUT pair copy plan",
                            prefix=prefix, before_placement=True)
        self.reject({"steps": [step(candidate=-1)]},
                    "A placed reduction listing cannot precede LUT pair copy plan",
                    environment={"NEXTPNR_MISTRAL_PLACED_REDUCTION": "absent_root 6 -1"},
                    before_placement=True)

    def test_fresh_pack_and_ordinary_heap_placement_required(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--fes-scaffold",), ("--fes-cart", "missing-cart.json"), ("--placer", "sa")):
            with self.subTest(options=options):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement",
                            options=options, before_placement=True)
        for loaded in ("pack", "place", "route"):
            with self.subTest(loaded=loaded):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement",
                            loaded_step=loaded, options=("--force",), before_placement=True)

    def test_effective_loaded_sa_is_rechecked_before_placement(self):
        log = self.reject({"steps": [step(candidate=-1)]}, "ordinary HeAP placement",
                          placer="sa", options=("--placer", "heap"), before_placement=True)
        self.assertNotIn("Running simulated annealing placer", log)

    def test_selected_failure_stops_before_next_step_and_route_even_with_force(self):
        for count in (1, 2):
            with self.subTest(count=count):
                log = self.reject({"steps": [step()] * count}, "did not qualify; routing was not started",
                                  route=True, options=("--force", "--rbf", "output.rbf"))
                self.assertIn("LUT pair copy plan step 0", log)
                self.assertIn("LUT pair copy discovery: 0 bounded cones.", log)
                self.assertNotIn("LUT pair copy plan step 1", log)
                self.assertNotIn("LUT pair copy applied candidate", log)

    def test_selected_plan_can_precede_driver_without_running_it_after_failure(self):
        log = self.reject({"steps": [step()]}, "did not qualify; routing was not started",
                          driver=True, route=True, options=("--force",))
        self.assertIn("LUT pair copy plan step 0", log)
        self.assertNotIn("cannot precede", log)
        self.assertNotIn("LUT driver copy discovery", log)

    def test_failed_earlier_selected_plan_prevents_copy_plan(self):
        for prefix in ("local-plan", "post-plan"):
            with self.subTest(prefix=prefix):
                log = self.reject({"steps": [step()]}, "did not qualify; routing was not started",
                                  prefix=prefix, prefix_candidate=0, options=("--force",))
                self.assertNotIn("LUT pair copy plan step", log)
                self.assertNotIn("LUT pair copy discovery", log)

    def test_disabled_and_saved_settings_do_not_activate_plan(self):
        settings = {"lut_pair_copy_plan": "missing.json", "lut_pair_copy_plan_list_only": 1,
                    "mistral/lutPairCopyPlan": "missing.json", "mistral/lutPairCopyPlanListOnly": 1}
        code, log, outputs = self.run_plan(enabled=False, settings=settings)
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"}, log)
        self.assertNotIn("LUT pair copy plan step", log)
        self.assertNotIn("LUT pair copy discovery", log)
        self.assertNotIn("LUT pair placement discovery", log)

    def test_plan_configuration_is_not_serialized_and_reload_does_not_activate_it(self):
        code, log, outputs, documents = self.run_plan({"steps": [step(candidate=-1)]}, collect=True)
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"}, log)
        saved = documents["output.json"]["modules"]["top"].get("settings", {})
        self.assertTrue(all("lutPairCopyPlan" not in name and "lut_pair_copy_plan" not in name
                            for name in saved), saved)
        self.assertNotIn("guidance/timing.json", json.dumps(saved))
        answer = self.run_plan({"steps": [step(candidate=-1)]}, reload=True)
        self.assertEqual(answer[0], 0, answer[1])
        self.assertEqual(len(answer), 6, answer)
        self.assertEqual(answer[3], 0, answer[4])
        self.assertTrue(answer[5])
        self.assertNotIn("LUT pair copy plan step", answer[4])
        self.assertNotIn("LUT pair copy discovery", answer[4])
        self.assertNotIn("LUT pair placement discovery", answer[4])


if __name__ == "__main__":
    unittest.main()
