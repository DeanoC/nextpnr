#!/usr/bin/env python3
"""Exercise explicit internal-cut plans through the CLI and JSON loader."""
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


class CombRemapPlanCliTest(unittest.TestCase):
    def run_plan(self, plan=None, *, raw=None, options=(), route=False, placer=None,
                 loaded_step=None, reports=None, local_plan=None, absolute=False,
                 reload=False):
        with tempfile.TemporaryDirectory(prefix="comb-remap-plan-cli-") as directory:
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
                path = root / "plans" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            if absolute:
                plan = {"steps": [dict(entry, report=str(root / "plans" / entry["report"]))
                                  for entry in plan["steps"]]}
            (root / "plans/plan.json").write_text(raw if raw is not None else json.dumps(plan))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--remap-comb-plan", str(root / "plans/plan.json"),
                       "--write", str(root / "output.json"), "--report", str(root / "output-report.json")]
            if not route:
                command += ["--no-route"]
            if local_plan is not None:
                (root / "plans/local.json").write_text(json.dumps(local_plan))
                command += ["--remap-plan", str(root / "plans/local.json")]
            # Report paths belong to the plan directory, not the subprocess cwd.
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command + list(options), cwd=root, env=env, timeout=30,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            output = any((root / name).exists() for name in
                         ("output.json", "output-report.json", "output.rbf"))
            answer = (result.returncode, result.stdout, output)
            if reload and result.returncode == 0:
                command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "output.json"),
                           "--no-pack", "--no-place", "--no-route", "--write", str(root / "reloaded.json")]
                again = subprocess.run(command, cwd=root, env=env, timeout=30,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                return answer + (again.returncode, again.stdout, (root / "reloaded.json").exists())
            return answer

    def reject(self, plan, text, **kwargs):
        code, log, output = self.run_plan(plan, **kwargs)
        self.assertNotEqual(code, 0, log)
        self.assertIn(text, log)
        self.assertFalse(output, log)
        self.assertNotIn("Routing...", log)
        return log

    def test_relative_absolute_and_normalized_report_paths_with_final_listing(self):
        for report, absolute in (("guidance/timing.json", False),
                                 ("guidance/../guidance/timing.json", False),
                                 ("guidance/timing.json", True)):
            with self.subTest(report=report, absolute=absolute):
                code, log, output = self.run_plan({"steps": [step(report=report, candidate=-1)]},
                                                 absolute=absolute)
                self.assertEqual(code, 0, log)
                self.assertIn("Comb-remap plan step 0: candidate=-1.", log)
                self.assertIn("Comb remap: 0 qualified candidates", log)
                self.assertTrue(output)

    def test_escaped_string_contents_are_not_scanned_as_keys(self):
        report = 'guidance/{"candidate":0},[steps].json'
        code, log, output = self.run_plan({"steps": [step(report=report, candidate=-1)]},
                                         reports={report: '{"critical_paths": []}'})
        self.assertEqual(code, 0, log)
        self.assertTrue(output)

    def test_selected_failure_stops_even_with_force_and_rbf_requested(self):
        log = self.reject({"steps": [step()]}, "did not qualify; routing was not started",
                          options=("--force", "--rbf", "output.rbf"), route=True)
        self.assertIn("Comb-remap plan step 0: candidate=0.", log)

    def test_all_report_files_and_syntax_preloaded_before_placement(self):
        for report, content, expected in (("missing.json", None, "comb-remap plan report"),
                                         ("invalid.json", "{", "Invalid comb-remap plan report"),
                                         ("invalid.json", '{"critical_paths": 3}',
                                          "Invalid comb-remap plan report")):
            with self.subTest(content=content):
                log = self.reject({"steps": [step(), step(report=report)]}, expected,
                                  reports={} if content is None else {report: content})
                self.assertNotIn("Comb-remap plan step", log)
                self.assertNotIn("Running analytical placer", log)

    def test_strict_root_and_step_schema(self):
        for plan in (None, [], {}, {"steps": None}, {"steps": {}}, {"steps": []},
                     {"steps": [step()] * 9}, {"steps": [step()], "unknown": 1},
                     {"steps": [None]}, {"steps": [{"report": "guidance/timing.json"}]},
                     {"steps": [{"candidate": 0}]}, {"steps": [step(unknown=True)]}):
            with self.subTest(plan=plan):
                self.reject(plan, "comb-remap plan")
        for raw in ("{", '{"steps": [', '{"steps": []} trailing'):
            with self.subTest(raw=raw):
                self.reject(None, "comb-remap plan", raw=raw)

    def test_candidate_is_a_finite_bounded_integer_before_cast(self):
        for value in (-2, -(2**40), 2**31, 0.5, True, "0", None):
            with self.subTest(value=value):
                self.reject({"steps": [step(candidate=value)]}, "Invalid integer")
        for literal in ("1e999", "-1e999", "NaN", "Infinity"):
            with self.subTest(literal=literal):
                raw = '{"steps":[{"report":"guidance/timing.json","candidate":' + literal + '}]}'
                self.reject(None, "comb-remap plan", raw=raw)

    def test_report_paths_have_exact_string_type_and_no_nul(self):
        for value in ("", "a\x00b", 1, True, None, []):
            with self.subTest(value=value):
                self.reject({"steps": [step(report=value)]}, "Invalid comb-remap plan path")

    def test_duplicate_plain_and_escaped_keys_are_rejected(self):
        item = json.dumps(step())
        for raw in ('{"steps":[' + item + '],"steps":[' + item + ']}',
                    '{"steps":[' + item + '],"st\\u0065ps":[' + item + ']}',
                    '{"steps":[{"report":"guidance/timing.json","candidate":0,"candidate":1}]}',
                    '{"steps":[{"report":"guidance/timing.json","candidate":0,"candi\\u0064ate":1}]}'):
            with self.subTest(raw=raw):
                self.reject(None, "Duplicate", raw=raw)

    def test_listing_is_final_and_cannot_route_or_write_rbf(self):
        self.reject({"steps": [step(candidate=-1), step()]}, "listing step must be last")
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last", route=True)
        self.reject({"steps": [step(candidate=-1)]}, "listing step must be last",
                    options=("--rbf", "output.rbf"))

    def test_requires_fresh_full_heap_placement(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--fes-scaffold",), ("--placer", "sa")):
            with self.subTest(options=options):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement", options=options)
        for loaded in ("pack", "place", "route"):
            with self.subTest(loaded=loaded):
                self.reject({"steps": [step()]}, "fresh ordinary HeAP placement", loaded_step=loaded)

    def test_effective_loaded_sa_rejected_even_with_cli_heap(self):
        self.reject({"steps": [step(candidate=-1)]}, "ordinary full-design HeAP placement",
                    placer="sa", options=("--placer", "heap"))

    def test_legacy_comb_options_cannot_mix_with_plan(self):
        for options in (("--remap-comb-candidate", "0"),
                        ("--remap-comb-critical", "plans/guidance/timing.json")):
            with self.subTest(options=options):
                self.reject({"steps": [step()]}, "cannot be combined with legacy", options=options)

    def test_legacy_or_staged_local_listing_cannot_precede_comb_plan(self):
        self.reject({"steps": [step(candidate=-1)]}, "Local-remap listing must be final",
                    options=("--remap-critical", "plans/guidance/timing.json"))
        local = {"steps": [dict(report="guidance/timing.json", candidate=-1, groups=1,
                                optimize_pins=False, preserve_ff_placement=True)]}
        self.reject({"steps": [step(candidate=-1)]}, "Local-remap listing must be final", local_plan=local)

    def test_selected_local_prefix_is_accepted_and_still_stops_on_its_failure(self):
        # Empty designs cannot qualify a selected local stage. Reaching its
        # execution header establishes that this combination passed CLI guards.
        local = {"steps": [dict(report="guidance/timing.json", candidate=0, groups=1,
                                optimize_pins=False, preserve_ff_placement=True)]}
        log = self.reject({"steps": [step(candidate=-1)]}, "did not qualify; routing was not started",
                          local_plan=local)
        self.assertIn("Local-remap plan step 0", log)
        self.assertNotIn("Comb-remap plan step", log)
        self.assertNotIn("cannot precede", log)

    def test_reloaded_listing_output_does_not_replay_a_cli_only_plan(self):
        answer = self.run_plan({"steps": [step(candidate=-1)]}, reload=True)
        self.assertEqual(answer[0], 0, answer[1])
        self.assertEqual(len(answer), 6, answer)
        _, _, output, code, log, reloaded = answer
        self.assertTrue(output)
        self.assertEqual(code, 0, log)
        self.assertTrue(reloaded)
        self.assertNotIn("Comb-remap plan step", log)


if __name__ == "__main__":
    unittest.main()
