#!/usr/bin/env python3
"""Exercise the guarded capture-pipeline placement request through its real CLI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
PIPELINE_ENV = "NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY"
NO_ELIGIBLE = "Capture pipeline locality: no eligible critical registered hard-IP source."
INVALID_SPEC = "Capture pipeline locality requires exactly report, budget 1..64 and radius 1..24."


class CapturePipelineLocalityCliTest(unittest.TestCase):
    def run_design(self, *, enabled=True, spec="{report} 1 24", options=(), route=False,
                   loaded_step=None, placer=None, settings=None,
                   report='{"critical_paths": []}', prefix=None, prefix_candidate=-1,
                   environment=None, reload=False):
        with tempfile.TemporaryDirectory(prefix="capture-pipeline-locality-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if loaded_step is not None:
                module["attributes"]["step"] = loaded_step
            if settings is not None:
                module["settings"] = dict(settings)
            if placer is not None:
                module.setdefault("settings", {})["placer"] = placer
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            if report is not None:
                (root / "timing.json").write_text(report)
            (root / "prefix-timing.json").write_text('{"critical_paths": []}')
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--write", str(root / "output.json")]
            if not route:
                command += ["--no-route"]
            else:
                # A failed rejection must not turn a regression test into a GPU job.
                command += ["--router", "router1"]
            if prefix in ("local", "comb"):
                critical, candidate = {
                    "local": ("--remap-critical", "--remap-candidate"),
                    "comb": ("--remap-comb-critical", "--remap-comb-candidate"),
                }[prefix]
                command += [critical, str(root / "prefix-timing.json"), candidate, str(prefix_candidate)]
            elif prefix in ("local-plan", "comb-plan"):
                step = dict(report="prefix-timing.json", candidate=prefix_candidate)
                if prefix == "local-plan":
                    step.update(groups=1, optimize_pins=False, preserve_ff_placement=True)
                (root / "prefix-plan.json").write_text(json.dumps({"steps": [step]}))
                option = {"local-plan": "--remap-plan", "comb-plan": "--remap-comb-plan"}[prefix]
                command += [option, str(root / "prefix-plan.json")]
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("NEXTPNR_MISTRAL_")}
            if enabled:
                env[PIPELINE_ENV] = spec.format(report=str(root / "timing.json"))
            env.update(environment or {})
            result = subprocess.run(command + list(options), cwd=root, env=env, timeout=45,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            outputs = {name for name in ("output.json", "output.rbf") if (root / name).exists()}
            answer = dict(code=result.returncode, log=result.stdout, outputs=outputs)
            if (root / "output.json").exists():
                answer["design"] = json.loads((root / "output.json").read_text())
            if reload and result.returncode == 0:
                # Persisted placement provenance must not restore this ENV request.
                reload_env = {key: value for key, value in env.items()
                              if not key.startswith("NEXTPNR_MISTRAL_")}
                again = subprocess.run(
                    [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "output.json"),
                     "--no-pack", "--no-place", "--no-route", "--write", str(root / "reloaded.json")],
                    cwd=root, env=reload_env, timeout=45, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True)
                answer["reload"] = dict(code=again.returncode, log=again.stdout,
                                        output=(root / "reloaded.json").exists())
            return answer

    def assert_unrouted(self, log):
        self.assertNotIn("Routing complete.", log)
        self.assertNotIn("Running the GPU router", log)

    def reject(self, reason, *, before_pack=False, before_placement=False, **kwargs):
        answer = self.run_design(**kwargs)
        log = answer["log"]
        self.assertNotEqual(answer["code"], 0, log)
        self.assertIn(reason, log)
        self.assertFalse(answer["outputs"], log)
        self.assert_unrouted(log)
        self.assertNotIn(NO_ELIGIBLE, log)
        self.assertNotIn("Capture pipeline locality chain", log)
        self.assertNotIn("Capture pipeline locality trial", log)
        self.assertNotIn("Capture pipeline locality retained", log)
        if before_pack:
            # The ordinary CLI prints utilisation immediately after its pack stage.
            self.assertNotIn("Device utilisation:", log)
        if before_pack or before_placement:
            self.assertNotIn("Creating initial analytic placement", log)
            self.assertNotIn("Running main analytical placer", log)
            self.assertNotIn("HeAP Placer Time:", log)
            self.assertNotIn("Running simulated annealing placer", log)
        return log

    def assert_empty_report_success(self, answer):
        log = answer["log"]
        self.assertEqual(answer["code"], 0, log)
        self.assertEqual(answer["outputs"], {"output.json"}, log)
        self.assertIn("HeAP Placer Time:", log)
        self.assertIn(NO_ELIGIBLE, log)
        self.assert_unrouted(log)
        module = answer["design"]["modules"]["top"]
        self.assertEqual(module["attributes"]["step"], "place")
        self.assertEqual(module["settings"]["placer"], "heap")
        for name, net in module["netnames"].items():
            # Empty string properties receive a trailing space in saved JSON.
            self.assertFalse(net["attributes"].get("ROUTING", "").strip(), name)

    def test_default_off_and_serialized_settings_do_not_activate(self):
        saved = {
            "capture_pipeline_report": "missing-timing.json",
            "capture_pipeline_budget": 64,
            "capture_pipeline_radius": 24,
            "mistral/capturePipelineLocality": "missing-timing.json 64 24",
        }
        for settings in (None, saved):
            with self.subTest(settings=settings):
                answer = self.run_design(enabled=False, settings=settings, report=None)
                self.assertEqual(answer["code"], 0, answer["log"])
                self.assertEqual(answer["outputs"], {"output.json"}, answer["log"])
                self.assertNotIn("Capture pipeline locality", answer["log"])
                self.assert_unrouted(answer["log"])

    def test_empty_report_succeeds_and_reload_does_not_inherit_request(self):
        answer = self.run_design(placer="heap", reload=True)
        self.assert_empty_report_success(answer)
        self.assertIn("reload", answer, answer)
        reloaded = answer["reload"]
        self.assertEqual(reloaded["code"], 0, reloaded["log"])
        self.assertTrue(reloaded["output"], reloaded["log"])
        self.assertNotIn("Capture pipeline locality", reloaded["log"])
        self.assert_unrouted(reloaded["log"])

    def test_exact_three_token_spec_and_integer_syntax_before_pack(self):
        for spec in ("", "{report}", "{report} 1", "{report} 1 1 extra",
                     "{report} word 1", "{report} 1 word", "{report} 1.5 1", "{report} 1 1.5"):
            with self.subTest(spec=spec):
                self.reject(INVALID_SPEC, spec=spec, before_pack=True, options=("--force",))

    def test_budget_and_radius_bounds_before_pack_and_valid_endpoints(self):
        for budget, radius in ((0, 1), (-1, 1), (65, 1), (1, 0), (1, -1), (1, 25)):
            with self.subTest(budget=budget, radius=radius):
                self.reject(INVALID_SPEC, spec="{report} %s %s" % (budget, radius),
                            before_pack=True, options=("--force",))
        for budget, radius in ((1, 1), (64, 24)):
            with self.subTest(budget=budget, radius=radius):
                self.assert_empty_report_success(
                    self.run_design(spec="{report} %s %s" % (budget, radius)))

    def test_report_is_read_and_top_level_validated_before_pack(self):
        self.reject("Cannot read capture pipeline locality timing report.", report=None,
                    before_pack=True, options=("--force",))
        for report in ("", "{", "[]", "null", "{}", '{"critical_paths": null}',
                       '{"critical_paths": 3}', '{"critical_paths": {}}'):
            with self.subTest(report=report):
                self.reject("Invalid capture pipeline locality timing report.", report=report,
                            before_pack=True, options=("--force",))

    def test_fresh_pack_place_and_non_fes_guards_even_with_force(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",),
                        ("--placer", "sa"), ("--fes-scaffold",),
                        ("--fes-cart", "missing-cart.json")):
            with self.subTest(options=options):
                self.reject("Capture pipeline locality requires fresh ordinary HeAP placement.",
                            options=options + ("--force",), before_pack=True)

    def test_loaded_steps_are_rejected_before_pack_even_with_force(self):
        for step in ("pack", "place", "route"):
            with self.subTest(step=step):
                self.reject("Capture pipeline locality requires fresh ordinary HeAP placement.",
                            loaded_step=step, options=("--force",), before_pack=True)

    def test_single_capture_environment_conflicts_even_when_empty(self):
        for value in ("", "absent_capture 1 24"):
            with self.subTest(value=value):
                self.reject("Capture pipeline locality cannot combine with single-capture locality.",
                            environment={"NEXTPNR_MISTRAL_CAPTURE_LOCALITY": value},
                            options=("--force",), before_pack=True)

    def test_effective_serialized_sa_placer_is_checked_before_heap(self):
        self.reject("Capture pipeline locality requires ordinary HeAP placement.", placer="sa",
                    options=("--placer", "heap", "--force"), before_placement=True)

    def test_earlier_local_and_comb_listings_are_rejected_before_heap(self):
        for prefix in ("local", "comb", "local-plan", "comb-plan"):
            with self.subTest(prefix=prefix):
                log = self.reject("An earlier remap listing cannot precede capture pipeline locality.",
                                  prefix=prefix, options=("--force",), before_placement=True)
                self.assertNotIn("Local-remap plan step", log)
                self.assertNotIn("Comb-remap plan step", log)

    def test_earlier_selected_failure_stops_before_pipeline_and_routing(self):
        for prefix, reason in (
                ("local", "Requested local-remap candidate was not qualified; routing was not started"),
                ("comb", "Requested comb-remap candidate was not qualified; routing was not started"),
                ("local-plan", "did not qualify; routing was not started"),
                ("comb-plan", "did not qualify; routing was not started")):
            with self.subTest(prefix=prefix):
                log = self.reject(reason, prefix=prefix, prefix_candidate=0, route=True,
                                  options=("--force", "--rbf", "output.rbf"))
                self.assertIn("HeAP Placer Time:", log)
                self.assertNotIn("Capture pipeline locality:", log)


if __name__ == "__main__":
    unittest.main()
