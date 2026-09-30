#!/usr/bin/env python3
"""Check optional driver-copy stage ordering through the actual CLI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class LutDriverCopyCliTest(unittest.TestCase):
    def run_design(self, *, options=(), copy=True, route=False, loaded_step=None,
                   placer=None, report='{"critical_paths": []}', prefix=None,
                   environment=None, reload=False):
        with tempfile.TemporaryDirectory(prefix="lut-driver-copy-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if loaded_step is not None:
                module["attributes"]["step"] = loaded_step
            if placer is not None:
                module["settings"] = {"placer": placer}
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            if report is not None:
                (root / "timing.json").write_text(report)
            (root / "prefix-timing.json").write_text('{"critical_paths": []}')
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--write", str(root / "output.json")]
            if not route:
                command += ["--no-route"]
            if copy:
                command += ["--remap-lut-driver-critical", str(root / "timing.json")]
            if prefix in ("local", "comb", "decomposition"):
                option, candidate = {
                    "local": ("--remap-critical", "--remap-candidate"),
                    "comb": ("--remap-comb-critical", "--remap-comb-candidate"),
                    "decomposition": ("--remap-decompose-critical", "--remap-decompose-candidate"),
                }[prefix]
                command += [option, str(root / "prefix-timing.json"), candidate, "-1"]
            elif prefix in ("local-plan", "comb-plan"):
                step = dict(report="prefix-timing.json", candidate=-1)
                if prefix == "local-plan":
                    step.update(groups=1, optimize_pins=False, preserve_ff_placement=True)
                (root / "prefix-plan.json").write_text(json.dumps({"steps": [step]}))
                command += ["--remap-plan" if prefix == "local-plan" else "--remap-comb-plan",
                            str(root / "prefix-plan.json")]
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
            return answer

    def reject(self, reason, *, before_placement=False, **kwargs):
        code, log, outputs = self.run_design(**kwargs)
        self.assertNotEqual(code, 0, log)
        self.assertIn(reason, log)
        self.assertFalse(outputs, log)
        self.assertNotIn("Routing...", log)
        if before_placement:
            self.assertNotIn("Running analytical placer", log)
            self.assertNotIn("LUT driver copy discovery", log)
        return log

    def test_option_dependency_and_negative_candidate(self):
        self.reject("--remap-lut-driver-critical", copy=False,
                    options=("--remap-lut-driver-candidate", "0"), before_placement=True)
        self.reject("candidate index", options=("--remap-lut-driver-candidate", "-2"),
                    before_placement=True)

    def test_listing_produces_reviewable_output_without_route(self):
        code, log, outputs = self.run_design(placer="heap")
        self.assertEqual(code, 0, log)
        self.assertIn("LUT driver copy discovery: 0 bounded data edges", log)
        self.assertEqual(outputs, {"output.json"}, log)
        self.assertNotIn("Routing...", log)

    def test_listing_requires_no_route_and_forbids_rbf(self):
        self.reject("listing requires --no-route and no --rbf", route=True,
                    before_placement=True)
        self.reject("listing requires --no-route and no --rbf",
                    options=("--rbf", "output.rbf"), before_placement=True)

    def test_fresh_pack_and_placement_required(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--fes-scaffold",), ("--placer", "sa")):
            with self.subTest(options=options):
                self.reject("fresh ordinary HeAP placement", options=options, before_placement=True)
        for step in ("pack", "place", "route"):
            with self.subTest(step=step):
                self.reject("fresh ordinary HeAP placement", loaded_step=step, before_placement=True)

    def test_effective_loaded_placer_is_rechecked(self):
        log = self.reject("requires ordinary HeAP placement", placer="sa", options=("--placer", "heap"))
        self.assertNotIn("Running simulated annealing placer", log)
        self.assertNotIn("LUT driver copy discovery", log)

    def test_report_is_loaded_and_parsed_before_placement(self):
        for report in (None, "{", "[]", '{"critical_paths": 3}'):
            with self.subTest(report=report):
                self.reject("timing report", report=report, before_placement=True)
        self.reject("Duplicate LUT driver copy report",
                    report='{"critical_paths": [], "critical_paths": []}', before_placement=True)

    def test_other_listing_must_be_final(self):
        for prefix in ("local", "comb", "local-plan", "comb-plan", "decomposition"):
            with self.subTest(prefix=prefix):
                self.reject("A remap listing must be final", prefix=prefix, before_placement=True)
        self.reject("A placed reduction listing cannot precede LUT driver copy",
                    environment={"NEXTPNR_MISTRAL_PLACED_REDUCTION": "absent_root 6 -1"})

    def test_selected_failure_stops_before_route_even_with_force(self):
        log = self.reject("Requested LUT driver copy candidate was not qualified; routing was not started",
                          route=True, options=("--remap-lut-driver-candidate", "0", "--force", "--rbf", "output.rbf"))
        self.assertIn("LUT driver copy discovery: 0 bounded data edges", log)

    def test_disabled_and_reloaded_stages_are_not_inherited(self):
        code, log, outputs = self.run_design(copy=False)
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"})
        self.assertNotIn("LUT driver copy discovery", log)
        answer = self.run_design(reload=True)
        self.assertEqual(answer[0], 0, answer[1])
        self.assertEqual(len(answer), 6, answer)
        _, _, _, code, log, reloaded = answer
        self.assertEqual(code, 0, log)
        self.assertTrue(reloaded)
        self.assertNotIn("LUT driver copy discovery", log)


if __name__ == "__main__":
    unittest.main()
