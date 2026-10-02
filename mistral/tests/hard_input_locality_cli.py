#!/usr/bin/env python3
"""Default-off hard-input requests and prefix ordering through the real CLI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ENV = "NEXTPNR_MISTRAL_HARD_INPUT_LOCALITY"
SUMMARY = "Hard input locality: eligible=0 attempted=0 retained=0"


class HardInputLocalityCliTest(unittest.TestCase):
    def run_design(self, *, enabled=True, spec="{report} 1 24", report='{"critical_paths": []}',
                   options=(), settings=None, step=None, prefix=None, selected=-1,
                   environment=None, reload=False):
        with tempfile.TemporaryDirectory(prefix="hard-input-locality-cli-") as directory:
            root = Path(directory)
            module = dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            if step is not None:
                module["attributes"]["step"] = step
            if settings is not None:
                module["settings"] = dict(settings)
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            if report is not None:
                (root / "guide.json").write_text(report)
            (root / "prefix.json").write_text('{"critical_paths": []}')
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--write", str(root / "output.json"), "--no-route"]
            if prefix in ("local", "comb", "decomposition", "pair", "driver"):
                flags = {
                    "local": ("--remap-critical", "--remap-candidate"),
                    "comb": ("--remap-comb-critical", "--remap-comb-candidate"),
                    "decomposition": ("--remap-decompose-critical", "--remap-decompose-candidate"),
                    "pair": ("--remap-lut-pair-critical", "--remap-lut-pair-candidate"),
                    "driver": ("--remap-lut-driver-critical", "--remap-lut-driver-candidate"),
                }[prefix]
                command += [flags[0], str(root / "prefix.json"), flags[1], str(selected)]
            elif prefix in ("local-plan", "comb-plan", "post-plan"):
                row = dict(report="prefix.json", candidate=selected)
                if prefix != "comb-plan":
                    row.update(groups=1, optimize_pins=False, preserve_ff_placement=True)
                (root / "plan.json").write_text(json.dumps({"steps": [row]}))
                flag = {"local-plan": "--remap-plan", "comb-plan": "--remap-comb-plan",
                        "post-plan": "--remap-post-plan"}[prefix]
                command += [flag, str(root / "plan.json")]
            env = {k: v for k, v in os.environ.items() if not k.startswith("NEXTPNR_MISTRAL_")}
            if enabled:
                env[ENV] = spec.format(report=str(root / "guide.json"))
            env.update({k: v.replace("{prefix}", str(root / "prefix.json"))
                           .replace("{report}", str(root / "guide.json"))
                        for k, v in (environment or {}).items()})
            result = subprocess.run(command + list(options), cwd=root, env=env, timeout=45,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            answer = dict(code=result.returncode, log=result.stdout,
                          outputs={p.name for p in root.glob("output.*")})
            if (root / "output.json").exists():
                answer["design"] = json.loads((root / "output.json").read_text())
            if reload and result.returncode == 0:
                clean = {k: v for k, v in env.items() if not k.startswith("NEXTPNR_MISTRAL_")}
                again = subprocess.run(
                    [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "output.json"),
                     "--no-pack", "--no-place", "--no-route", "--write", str(root / "reloaded.json")],
                    cwd=root, env=clean, timeout=45, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True)
                answer["reload"] = dict(code=again.returncode, log=again.stdout,
                                        output=(root / "reloaded.json").exists())
            return answer

    def no_routing(self, log):
        self.assertNotIn("Running the GPU router", log)
        self.assertNotIn("Routing complete.", log)

    def reject(self, reason, *, before_pack=True, **kwargs):
        answer = self.run_design(**kwargs)
        log = answer["log"]
        self.assertNotEqual(answer["code"], 0, log)
        self.assertIn(reason, log)
        self.assertFalse(answer["outputs"], log)
        self.assertNotIn("Hard input locality trial ", log)
        self.assertNotIn("Hard input locality retained ", log)
        self.assertNotIn(SUMMARY, log)
        self.no_routing(log)
        if before_pack:
            self.assertNotIn("Device utilisation:", log)
            self.assertNotIn("HeAP Placer Time:", log)
        return log

    def test_default_off_including_saved_settings(self):
        for settings in (None, {"hard_input_report": "missing.json", "hard_input_budget": 64,
                                "hard_input_radius": 24, "mistral/hardInputLocality": "missing.json 64 24"}):
            with self.subTest(settings=settings):
                answer = self.run_design(enabled=False, report=None, settings=settings)
                self.assertEqual(answer["code"], 0, answer["log"])
                self.assertEqual(answer["outputs"], {"output.json"}, answer["log"])
                self.assertNotIn("Hard input locality", answer["log"])
                self.no_routing(answer["log"])

    def test_empty_current_guide_and_reload_are_ordinary_unrouted_placement(self):
        answer = self.run_design(reload=True)
        self.assertEqual(answer["code"], 0, answer["log"])
        self.assertIn("HeAP Placer Time:", answer["log"])
        self.assertIn(SUMMARY, answer["log"])
        self.assertEqual(answer["outputs"], {"output.json"})
        module = answer["design"]["modules"]["top"]
        self.assertEqual(module["attributes"]["step"], "place")
        self.assertEqual(module["settings"]["placer"], "heap")
        for net in module["netnames"].values():
            self.assertFalse(net["attributes"].get("ROUTING", "").strip())
        self.assertEqual(answer["reload"]["code"], 0, answer["reload"]["log"])
        self.assertTrue(answer["reload"]["output"])
        self.assertNotIn("Hard input locality", answer["reload"]["log"])
        self.no_routing(answer["log"])
        self.no_routing(answer["reload"]["log"])

    def test_exact_request_syntax_and_bounds_even_force(self):
        for spec in ("", "{report}", "{report} 1", "{report} 1 1 extra", "{report} 1.5 1",
                     "{report} 1 1.5", "{report} word 1", "{report} 0 24", "{report} 65 24",
                     "{report} 1 0", "{report} 1 25"):
            with self.subTest(spec=spec):
                self.reject("Hard input locality requires exactly report, budget 1..64 and radius 1..24.",
                            spec=spec, options=("--force",))
        for spec in ("{report} 1 1", "{report} 64 24"):
            answer = self.run_design(spec=spec)
            self.assertEqual(answer["code"], 0, answer["log"])
            self.assertIn(SUMMARY, answer["log"])

    def test_report_read_before_pack_and_malformed_top_level(self):
        self.reject("Cannot read hard input locality timing report.", report=None, options=("--force",))
        for report in ("", "{", "[]", "null", "{}", '{"critical_paths": null}', '{"critical_paths": {}}'):
            with self.subTest(report=report):
                self.reject("Invalid hard input locality timing report.", report=report, options=("--force",))

    def test_fresh_pack_ordinary_heap_and_loaded_steps_even_force(self):
        for options in (("--no-pack",), ("--no-place",), ("--pack-only",), ("--placer", "sa"),
                        ("--fes-scaffold",), ("--fes-cart", "missing.json")):
            with self.subTest(options=options):
                self.reject("Hard input locality requires fresh ordinary HeAP placement.", options=options + ("--force",))
        for step in ("pack", "place", "route"):
            with self.subTest(step=step):
                self.reject("Hard input locality requires fresh ordinary HeAP placement.", step=step, options=("--force",))
        self.reject("Hard input locality requires ordinary fresh HeAP placement.",
                    settings={"placer": "sa"}, options=("--placer", "heap", "--force"), before_pack=False)

    def test_single_capture_conflict_but_empty_pipeline_prefix_is_allowed(self):
        self.reject("Hard input locality cannot combine with single-capture locality.",
                    environment={"NEXTPNR_MISTRAL_CAPTURE_LOCALITY": ""}, options=("--force",))
        self.reject("Invalid capture pipeline locality timing report.",
                    environment={"NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY": "/dev/null 1 24"})
        answer = self.run_design(environment={"NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY": "{prefix} 1 24"})
        self.assertEqual(answer["code"], 0, answer["log"])
        self.assertIn("Capture pipeline locality: no eligible", answer["log"])
        self.assertIn(SUMMARY, answer["log"])
        self.assertLess(answer["log"].index("Capture pipeline locality: no eligible"), answer["log"].index(SUMMARY))
        self.no_routing(answer["log"])

    def test_all_earlier_listings_are_final_even_force(self):
        for prefix in ("local", "comb", "decomposition", "local-plan", "comb-plan", "post-plan", "pair", "driver"):
            with self.subTest(prefix=prefix):
                self.reject("listing", prefix=prefix, options=("--force",))
        self.reject("A placed reduction listing cannot precede", options=("--force",),
                    environment={"NEXTPNR_MISTRAL_PLACED_REDUCTION": "renamed_root 1 -1"})

    def test_selected_earlier_failures_stop_before_hard_worker_even_force(self):
        for prefix in ("local", "comb", "pair", "driver"):
            with self.subTest(prefix=prefix):
                log = self.reject("was not qualified; routing was not started", before_pack=False,
                                  prefix=prefix, selected=0, options=("--force",))
                self.assertIn("HeAP Placer Time:", log)
                self.assertNotIn("Hard input locality:", log)


if __name__ == "__main__":
    unittest.main()
