#!/usr/bin/env python3
"""Exercise the optional joint-LUT placement stage through its real CLI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class LutPairPlacementCliTest(unittest.TestCase):
    def run_design(self, *, options=(), pair=True, route=False, loaded_step=None,
                   placer=None, report='{"critical_paths": []}', prefix=None,
                   prefix_candidate=-1, driver=False, environment=None, reload=False):
        with tempfile.TemporaryDirectory(prefix="lut-pair-placement-cli-") as directory:
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
            if pair:
                command += ["--remap-lut-pair-critical", str(root / "timing.json")]
            if prefix in ("local", "comb", "decomposition"):
                critical, candidate = {
                    "local": ("--remap-critical", "--remap-candidate"),
                    "comb": ("--remap-comb-critical", "--remap-comb-candidate"),
                    "decomposition": ("--remap-decompose-critical", "--remap-decompose-candidate"),
                }[prefix]
                command += [critical, str(root / "prefix-timing.json"), candidate, str(prefix_candidate)]
            elif prefix in ("local-plan", "post-plan", "comb-plan"):
                step = dict(report="prefix-timing.json", candidate=prefix_candidate)
                if prefix != "comb-plan":
                    step.update(groups=1, optimize_pins=False, preserve_ff_placement=True)
                (root / "prefix-plan.json").write_text(json.dumps({"steps": [step]}))
                option = {"local-plan": "--remap-plan", "post-plan": "--remap-post-plan",
                          "comb-plan": "--remap-comb-plan"}[prefix]
                command += [option, str(root / "prefix-plan.json")]
            if driver:
                command += ["--remap-lut-driver-critical", str(root / "prefix-timing.json"),
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
            return answer

    def reject(self, reason, *, before_placement=False, **kwargs):
        code, log, outputs = self.run_design(**kwargs)
        self.assertNotEqual(code, 0, log)
        self.assertIn(reason, log)
        self.assertFalse(outputs, log)
        self.assertNotIn("Routing complete.", log)
        self.assertNotIn("Running the GPU router", log)
        if before_placement:
            self.assertNotIn("Running analytical placer", log)
            self.assertNotIn("LUT pair placement discovery", log)
        return log

    def test_option_dependency_and_negative_candidate(self):
        self.reject("--remap-lut-pair-candidate requires --remap-lut-pair-critical", pair=False,
                    options=("--remap-lut-pair-candidate", "0"), before_placement=True)
        self.reject("Invalid LUT pair placement candidate index", before_placement=True,
                    options=("--remap-lut-pair-candidate", "-2"))

    def test_default_listing_writes_reviewable_placement_without_route(self):
        code, log, outputs = self.run_design(placer="heap")
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"}, log)
        self.assertIn("LUT pair placement discovery: 0 bounded pairs", log)
        self.assertIn("LUT pair placement: 0 qualified candidates; no candidate applied.", log)
        self.assertNotIn("Routing complete.", log)
        self.assertNotIn("Running the GPU router", log)

    def test_listing_requires_no_route_and_forbids_rbf(self):
        self.reject("LUT pair placement listing requires --no-route and no --rbf", route=True,
                    before_placement=True)
        self.reject("LUT pair placement listing requires --no-route and no --rbf",
                    options=("--rbf", "output.rbf"), before_placement=True)

    def test_fresh_pack_place_and_non_fes_design_required(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--fes-scaffold",), ("--fes-cart", "missing-cart.json"), ("--placer", "sa")):
            with self.subTest(options=options):
                self.reject("LUT pair placement requires fresh ordinary HeAP placement",
                            options=options, before_placement=True)
        for step in ("pack", "place", "route"):
            with self.subTest(step=step):
                self.reject("LUT pair placement requires fresh ordinary HeAP placement",
                            loaded_step=step, before_placement=True)

    def test_effective_loaded_sa_placer_cannot_be_overridden_into_this_pass(self):
        log = self.reject("LUT pair placement requires ordinary HeAP placement", placer="sa",
                          options=("--placer", "heap"))
        self.assertNotIn("Running simulated annealing placer", log)
        self.assertNotIn("LUT pair placement discovery", log)

    def test_report_loaded_and_validated_before_placement(self):
        for report in (None, "{", "[]", '{"critical_paths": 3}'):
            with self.subTest(report=report):
                self.reject("timing report", report=report, before_placement=True)
        self.reject("Duplicate LUT pair placement report",
                    report='{"critical_paths": [], "critical_paths": []}', before_placement=True)

    def test_every_earlier_listing_must_be_final(self):
        for prefix in ("local", "comb", "decomposition", "local-plan", "comb-plan", "post-plan"):
            with self.subTest(prefix=prefix):
                self.reject("A remap listing must be final; it cannot precede LUT pair placement",
                            prefix=prefix, before_placement=True)
        self.reject("A placed reduction listing cannot precede LUT pair placement",
                    environment={"NEXTPNR_MISTRAL_PLACED_REDUCTION": "absent_root 6 -1"},
                    before_placement=True)

    def test_pair_listing_cannot_precede_even_selected_driver_copy(self):
        self.reject("A LUT pair placement listing must be final; it cannot precede LUT driver copy",
                    driver=True, before_placement=True)

    def test_selected_failure_stops_before_routing_even_with_force(self):
        log = self.reject("Requested LUT pair placement candidate was not qualified; routing was not started",
                          route=True, options=("--remap-lut-pair-candidate", "0", "--force",
                                               "--rbf", "output.rbf"))
        self.assertIn("LUT pair placement discovery: 0 bounded pairs", log)

    def test_unsatisfied_earlier_selected_stage_never_reaches_pair_search(self):
        for prefix in ("local-plan", "post-plan"):
            with self.subTest(prefix=prefix):
                log = self.reject("did not qualify; routing was not started", prefix=prefix, prefix_candidate=0,
                                  options=("--remap-lut-pair-candidate", "0", "--force"))
                self.assertNotIn("LUT pair placement discovery", log)

    def test_disabled_and_reloaded_designs_do_not_inherit_pair_selection(self):
        code, log, outputs = self.run_design(pair=False)
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"})
        self.assertNotIn("LUT pair placement discovery", log)
        answer = self.run_design(reload=True)
        self.assertEqual(answer[0], 0, answer[1])
        self.assertEqual(len(answer), 6, answer)
        _, _, _, code, log, reloaded = answer
        self.assertEqual(code, 0, log)
        self.assertTrue(reloaded)
        self.assertNotIn("LUT pair placement discovery", log)


if __name__ == "__main__":
    unittest.main()
