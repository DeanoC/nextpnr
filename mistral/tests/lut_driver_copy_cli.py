#!/usr/bin/env python3
"""Check optional driver-copy stage ordering through the actual CLI."""
import json
import math
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
                   environment=None, reload=False, design_module=None,
                   sdc=None, report_output=False, collect=False):
        with tempfile.TemporaryDirectory(prefix="lut-driver-copy-cli-") as directory:
            root = Path(directory)
            module = (dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
                      if design_module is None else json.loads(json.dumps(design_module)))
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
            if sdc is not None:
                (root / "clocks.sdc").write_text(sdc)
                command += ["--sdc", str(root / "clocks.sdc")]
            if report_output:
                command += ["--report", str(root / "predicted-report.json")]
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
            outputs = {name for name in ("output.json", "output.rbf", "predicted-report.json")
                       if (root / name).exists()}
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
                             for name in ("output.json", "predicted-report.json")
                             if (root / name).exists()}
                return answer + (documents,)
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

    def test_explicit_no_route_report_contains_placed_native_paths(self):
        # A loaded native placement bypasses the placer's private STA object.
        # Its explicit report must therefore analyse this final graph afresh.
        def ff(bel, datain, q):
            connections = {pin: ["x"] for pin in
                           ("ENA", "ACLR", "SCLR", "SLOAD", "SDATA")}
            connections.update(CLK=[2], DATAIN=[datain], Q=[q])
            return dict(type="MISTRAL_FF", parameters={},
                        attributes={"NEXTPNR_BEL": bel}, connections=connections,
                        port_directions={pin: "output" if pin == "Q" else "input"
                                         for pin in connections})

        def constant(bel, value, q):
            return dict(type="MISTRAL_CONST", parameters={"LUT": str(value)},
                        attributes={"NEXTPNR_BEL": bel}, connections={"Q": [q]},
                        port_directions={"Q": "output"})

        module = dict(
            attributes={"top": 1, "step": "place"}, ports={},
            cells={
                "launch": ff("MISTRAL_FF.24.20.2", 3, 3),
                "capture": ff("MISTRAL_FF.30.20.2", 5, 4),
                "$PACKER_VCC_DRV": constant("MISTRAL_COMB.24.20.0", 1, 6),
                "$PACKER_GND_DRV": constant("MISTRAL_COMB.24.20.1", 0, 7),
                "logic": dict(type="MISTRAL_ALUT2", parameters={"LUT": "0110"},
                              attributes={"NEXTPNR_BEL": "MISTRAL_COMB.30.20.0"},
                              connections={"A": [3], "B": [4], "Q": [5]},
                              port_directions={"A": "input", "B": "input", "Q": "output"}),
            },
            netnames={name: dict(bits=[bit], attributes={}) for name, bit in
                      (("clock", 2), ("launch_q", 3), ("capture_q", 4), ("logic_q", 5),
                       ("$PACKER_VCC_NET", 6), ("$PACKER_GND_NET", 7))},
        )
        common = dict(copy=False, design_module=module,
                      options=("--no-pack", "--no-place"),
                      sdc="create_clock -period 10 [get_nets clock]\n", collect=True)
        code, log, outputs, documents = self.run_design(report_output=True, **common)
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json", "predicted-report.json"}, log)
        self.assertIn("Running predicted placed timing analysis for --report", log)
        self.assertNotIn("LUT driver copy discovery", log)
        self.assertNotIn("Routing...", log)
        self.assertNotIn("Running the GPU router", log)

        timing = documents["predicted-report.json"]
        self.assertEqual(set(timing["fmax"]), {"clock"}, timing)
        self.assertTrue(math.isfinite(timing["fmax"]["clock"]["achieved"]))
        self.assertGreater(timing["fmax"]["clock"]["achieved"], 0)
        self.assertAlmostEqual(timing["fmax"]["clock"]["constraint"], 100, places=3)
        self.assertTrue(timing["critical_paths"], timing)
        path = timing["critical_paths"][0]["path"]
        self.assertTrue(path, timing)
        self.assertEqual(path[0]["type"], "clk-to-q")
        self.assertEqual(path[-1]["type"], "setup")
        self.assertEqual((path[0]["from"]["cell"], path[0]["from"]["port"]), ("launch", "Q"))
        self.assertEqual((path[-1]["to"]["cell"], path[-1]["to"]["port"]), ("capture", "DATAIN"))
        self.assertTrue(any(segment["type"] == "routing" and
                            segment["from"]["cell"] == "launch" and segment["from"]["port"] == "Q" and
                            segment["to"]["cell"] == "logic" and segment["to"]["port"] == "A"
                            for segment in path), timing)
        self.assertTrue(any(segment["type"] == "routing" and
                            segment["from"]["cell"] == "logic" and segment["from"]["port"] == "Q" and
                            segment["to"]["cell"] == "capture" and segment["to"]["port"] == "DATAIN"
                            for segment in path), timing)
        for critical in timing["critical_paths"]:
            self.assertTrue(math.isfinite(critical["max_delay"]))
            for segment in critical["path"]:
                self.assertTrue(math.isfinite(segment["delay"]))
                if segment["type"] != "setup":
                    self.assertGreaterEqual(segment["delay"], 0)
                self.assertIn(segment["from"]["cell"], module["cells"])
                self.assertIn(segment["to"]["cell"], module["cells"])

        placed = documents["output.json"]["modules"]["top"]
        self.assertEqual(placed["attributes"]["step"], "place")
        self.assertEqual(set(placed["cells"]), set(module["cells"]))
        for name, cell in module["cells"].items():
            self.assertEqual(placed["cells"][name]["attributes"]["NEXTPNR_BEL"],
                             cell["attributes"]["NEXTPNR_BEL"])
        self.assertTrue(all(not net.get("attributes", {}).get("ROUTING")
                            for net in placed["netnames"].values()))

        code, log, outputs, documents = self.run_design(**common)
        self.assertEqual(code, 0, log)
        self.assertEqual(outputs, {"output.json"}, log)
        self.assertNotIn("predicted placed timing analysis", log)
        self.assertNotIn("LUT driver copy discovery", log)
        self.assertNotIn("Routing...", log)
        self.assertNotIn("Running the GPU router", log)
        self.assertEqual(documents["output.json"]["modules"]["top"], placed)


if __name__ == "__main__":
    unittest.main()
