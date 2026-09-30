#!/usr/bin/env python3
"""Exercise decomposition guards through actual CLI and JSON loading."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class DecompositionCliTest(unittest.TestCase):
    def run_design(self, *, options=(), remap=True, route=False, placer=None,
                   loaded_step=None, report='{"critical_paths": []}', prefix=None,
                   prefix_selection=-1, environment=None, reload=False):
        with tempfile.TemporaryDirectory(prefix="decomposition-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if placer is not None:
                module["settings"] = {"placer": placer}
            if loaded_step is not None:
                module["attributes"]["step"] = loaded_step
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            if report is not None:
                (root / "timing.json").write_text(report)
            (root / "prefix-timing.json").write_text('{"critical_paths": []}')
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--write", str(root / "output.json"), "--report", str(root / "output-report.json")]
            if not route:
                command += ["--no-route"]
            if remap:
                command += ["--remap-decompose-critical", str(root / "timing.json")]
            if prefix in ("local", "comb"):
                option = "--remap-critical" if prefix == "local" else "--remap-comb-critical"
                candidate = "--remap-candidate" if prefix == "local" else "--remap-comb-candidate"
                command += [option, str(root / "prefix-timing.json"), candidate, str(prefix_selection)]
            elif prefix in ("local-plan", "comb-plan"):
                step = dict(report="prefix-timing.json", candidate=prefix_selection)
                if prefix == "local-plan":
                    step.update(groups=1, optimize_pins=False, preserve_ff_placement=True)
                (root / "prefix-plan.json").write_text(json.dumps({"steps": [step]}))
                command += ["--remap-plan" if prefix == "local-plan" else "--remap-comb-plan",
                            str(root / "prefix-plan.json")]
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            env.update(environment or {})
            result = subprocess.run(command + list(options), cwd=root, env=env, timeout=30,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            output = any((root / name).exists() for name in
                         ("output.json", "output-report.json", "output.rbf"))
            answer = (result.returncode, result.stdout, output)
            if reload and result.returncode == 0:
                again = subprocess.run(
                    [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "output.json"),
                     "--no-pack", "--no-place", "--no-route", "--write", str(root / "reloaded.json")],
                    cwd=root, env=env, timeout=30, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True)
                return answer + (again.returncode, again.stdout, (root / "reloaded.json").exists())
            return answer

    def reject(self, text, **kwargs):
        code, log, output = self.run_design(**kwargs)
        self.assertNotEqual(code, 0, log)
        self.assertIn(text, log)
        self.assertFalse(output, log)
        self.assertNotIn("Routing...", log)
        return log

    def test_candidate_requires_report(self):
        self.reject("--remap-decompose-candidate requires --remap-decompose-critical",
                    remap=False, options=("--remap-decompose-candidate", "0"))

    def test_negative_candidate_rejected_before_placement(self):
        log = self.reject("Invalid control decomposition candidate index",
                          options=("--remap-decompose-candidate", "-2"))
        self.assertNotIn("Running analytical placer", log)

    def test_heap_listing_runs_and_writes_reviewable_output(self):
        for options in ((), ("--remap-decompose-candidate", "-1")):
            with self.subTest(options=options):
                code, log, output = self.run_design(placer="heap", options=options)
                self.assertEqual(code, 0, log)
                self.assertIn("Decomposition discovery: 0 bounded seven-input cuts.", log)
                self.assertTrue(output)
                self.assertNotIn("Routing...", log)

    def test_listing_requires_no_route(self):
        self.reject("Control decomposition listing requires --no-route and no --rbf", route=True)

    def test_listing_forbids_rbf_even_with_no_route(self):
        self.reject("Control decomposition listing requires --no-route and no --rbf",
                    options=("--rbf", "output.rbf"))

    def test_fresh_pack_and_heap_placement_required(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--fes-scaffold",), ("--fes-cart", "absent-cart.json"),
                        ("--placer", "sa")):
            with self.subTest(options=options):
                self.reject("Control decomposition requires fresh ordinary HeAP placement", options=options)

    def test_loaded_pack_place_or_route_cannot_replay_a_stage(self):
        for loaded_step in ("pack", "place", "route"):
            with self.subTest(loaded_step=loaded_step):
                self.reject("Control decomposition requires fresh ordinary HeAP placement",
                            loaded_step=loaded_step)

    def test_effective_loaded_sa_cannot_be_overridden_by_cli_heap(self):
        log = self.reject("Control decomposition requires ordinary HeAP placement",
                          placer="sa", options=("--placer", "heap"))
        self.assertNotIn("Running simulated annealing placer", log)
        self.assertNotIn("Decomposition discovery", log)

    def test_report_file_and_syntax_preloaded_before_placement(self):
        for report, expected in ((None, "control decomposition timing report"),
                                 ("{", "Invalid control decomposition timing report"),
                                 ("[]", "Invalid control decomposition timing report"),
                                 ('{"critical_paths": 3}', "Invalid control decomposition timing report")):
            with self.subTest(report=report):
                log = self.reject(expected, report=report)
                self.assertNotIn("Running analytical placer", log)
                self.assertNotIn("Decomposition discovery", log)

    def test_legacy_local_or_comb_listing_must_be_final(self):
        for prefix in ("local", "comb"):
            with self.subTest(prefix=prefix):
                self.reject("A remap listing must be final; it cannot precede control decomposition", prefix=prefix)

    def test_staged_local_or_comb_listing_must_be_final(self):
        for prefix in ("local-plan", "comb-plan"):
            with self.subTest(prefix=prefix):
                self.reject("A remap listing must be final; it cannot precede control decomposition", prefix=prefix)

    def test_selected_prefix_is_allowed_and_fails_before_decomposition(self):
        markers = {"local": "Local remap:", "comb": "Comb remap:",
                   "local-plan": "Local-remap plan step", "comb-plan": "Comb-remap plan step"}
        for prefix, marker in markers.items():
            with self.subTest(prefix=prefix):
                log = self.reject("routing was not started",
                                  prefix=prefix, prefix_selection=0)
                self.assertIn(marker, log)
                self.assertNotIn("cannot precede", log)
                self.assertNotIn("Decomposition discovery", log)

    def test_selected_failure_stops_before_routing_even_with_force_and_rbf(self):
        log = self.reject("Requested decomposition candidate was not qualified; routing was not started",
                          route=True, options=("--remap-decompose-candidate", "0", "--force", "--rbf", "output.rbf"))
        self.assertIn("Decomposition discovery: 0 bounded seven-input cuts.", log)

    def test_runtime_placed_reduction_listing_cannot_precede_decomposition(self):
        log = self.reject("A placed reduction listing cannot precede control decomposition",
                          environment={"NEXTPNR_MISTRAL_PLACED_REDUCTION": "absent_root 6 -1"})
        self.assertNotIn("Decomposition discovery", log)

    def test_reloaded_output_does_not_serialize_the_cli_stage(self):
        answer = self.run_design(reload=True)
        self.assertEqual(answer[0], 0, answer[1])
        self.assertEqual(len(answer), 6, answer)
        _, _, output, code, log, reloaded = answer
        self.assertTrue(output)
        self.assertEqual(code, 0, log)
        self.assertTrue(reloaded)
        self.assertNotIn("Decomposition discovery", log)


if __name__ == "__main__":
    unittest.main()
