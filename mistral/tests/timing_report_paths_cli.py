#!/usr/bin/env python3
"""Exercise the optional native multi-endpoint report through the actual CLI."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


def placed_fixture():
    def ff(bel, datain, q):
        ports = {pin: ["x"] for pin in ("ENA", "ACLR", "SCLR", "SLOAD", "SDATA")}
        ports.update(CLK=[2], DATAIN=[datain], Q=[q])
        return dict(type="MISTRAL_FF", parameters={}, attributes={"NEXTPNR_BEL": bel},
                    connections=ports,
                    port_directions={pin: "output" if pin == "Q" else "input" for pin in ports})

    def constant(bel, value, q):
        return dict(type="MISTRAL_CONST", parameters={"LUT": str(value)},
                    attributes={"NEXTPNR_BEL": bel}, connections={"Q": [q]},
                    port_directions={"Q": "output"})

    # Native constants, their exact packer net names, and actual legal LABs
    # match the independently tested placed-report fixture.
    cells = {
        "launch": ff("MISTRAL_FF.24.20.2", 3, 3),
        "$PACKER_VCC_DRV": constant("MISTRAL_COMB.24.20.0", 1, 6),
        "$PACKER_GND_DRV": constant("MISTRAL_COMB.24.20.1", 0, 7),
    }
    nets = {name: dict(bits=[bit], attributes={}) for name, bit in
            (("clock", 2), ("launch_q", 3), ("$PACKER_VCC_NET", 6), ("$PACKER_GND_NET", 7))}
    # Same tile/delays, deliberately non-lexical creation order. Each LUT's
    # two signal inputs alias the launch net; its AND table remains nonconstant.
    for index, name in enumerate(("capture_zeta", "capture_beta", "capture_alpha")):
        q, data = 10 + index * 2, 11 + index * 2
        z = index * 6
        cells[name] = ff(f"MISTRAL_FF.30.20.{z + 2}", data, q)
        cells[name + "$logic"] = dict(
            type="MISTRAL_ALUT2", parameters={"LUT": "1000"},
            attributes={"NEXTPNR_BEL": f"MISTRAL_COMB.30.20.{z}"},
            connections={"A": [3], "B": [3], "Q": [data]},
            port_directions={"A": "input", "B": "input", "Q": "output"})
        nets[name + "$q"] = dict(bits=[q], attributes={})
        nets[name + "$data"] = dict(bits=[data], attributes={})
    return dict(attributes={"top": 1, "step": "place"}, ports={}, cells=cells, netnames=nets)


class TimingReportPathsCliTest(unittest.TestCase):
    def run_design(self, *, count=None, report=True, placed=True, detailed=False):
        with tempfile.TemporaryDirectory(prefix="timing-report-paths-cli-") as directory:
            root = Path(directory)
            module = placed_fixture() if placed else dict(attributes={"top": 1}, ports={}, cells={}, netnames={})
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            (root / "clock.sdc").write_text("create_clock -period 10 [get_nets clock]\n")
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(root / "design.json"),
                       "--no-route", "--write", str(root / "output.json")]
            if placed:
                command += ["--no-pack", "--no-place", "--sdc", str(root / "clock.sdc")]
            if report:
                command += ["--report", str(root / "timing.json")]
            if detailed:
                command += ["--detailed-timing-report"]
            if count is not None:
                command += ["--timing-report-paths", str(count)]
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("NEXTPNR_MISTRAL_")}
            result = subprocess.run(command, cwd=root, env=environment, timeout=45,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            outputs = {name: (root / name).read_text() for name in ("output.json", "timing.json")
                       if (root / name).exists()}
            return result.returncode, result.stdout, outputs

    def test_requires_report_even_for_explicit_default(self):
        for count in (1, 2):
            with self.subTest(count=count):
                code, log, outputs = self.run_design(count=count, report=False, placed=False)
                self.assertNotEqual(code, 0, log)
                self.assertIn("--timing-report-paths requires --report", log)
                self.assertFalse(outputs, log)
                self.assertNotIn("Packing", log)
                self.assertNotIn("Running analytical placer", log)

    def test_bounds_and_invalid_integer_stop_before_output(self):
        for count in (0, -1, 16385, "2147483648", "abc", "2.5"):
            with self.subTest(count=count):
                code, log, outputs = self.run_design(count=count, placed=False)
                self.assertNotEqual(code, 0, log)
                self.assertFalse(outputs, log)
                self.assertNotIn("Running analytical placer", log)
                if isinstance(count, int):
                    self.assertIn("--timing-report-paths must be between 1 and 16384", log)

    def test_default_and_explicit_one_have_identical_report_and_native_graph(self):
        a = self.run_design()
        b = self.run_design(count=1)
        for code, log, outputs in (a, b):
            self.assertEqual(code, 0, log)
            self.assertEqual(set(outputs), {"output.json", "timing.json"}, log)
            self.assertIn("Running predicted placed timing analysis", log)
            self.assertNotIn("additional registered setup endpoint", log)
            self.assertNotIn("Routing...", log)
            self.assertNotIn("Running the GPU router", log)
        self.assertEqual(a[2], b[2])
        timing = json.loads(a[2]["timing.json"])
        self.assertNotIn("timing_report_paths", timing)
        self.assertEqual(len(timing["critical_paths"]), 1, timing)

    def assert_native_paths(self, timing, placed):
        cells = placed["modules"]["top"]["cells"]
        endpoints = []
        self.assertEqual(set(timing["fmax"]), {"clock"})
        self.assertAlmostEqual(timing["fmax"]["clock"]["constraint"], 100, places=3)
        self.assertTrue(math.isfinite(timing["fmax"]["clock"]["achieved"]))
        self.assertGreater(timing["fmax"]["clock"]["achieved"], 0)
        for report in timing["critical_paths"]:
            self.assertEqual((report["from"], report["to"]), ("posedge clock", "posedge clock"))
            self.assertAlmostEqual(report["max_delay"], 10, places=6)
            path = report["path"]
            self.assertTrue(path)
            self.assertEqual(path[0]["type"], "clk-to-q")
            self.assertEqual((path[0]["from"]["cell"], path[0]["from"]["port"]), ("launch", "Q"))
            self.assertEqual(path[-1]["type"], "setup")
            self.assertEqual(path[-1]["to"]["port"], "DATAIN")
            endpoint = path[-1]["to"]["cell"]
            endpoints.append(endpoint)
            for segment in path:
                self.assertTrue(math.isfinite(segment["delay"]))
                for end in ("from", "to"):
                    terminal = segment[end]
                    self.assertIn(terminal["cell"], cells)
                    cell = cells[terminal["cell"]]
                    self.assertIn(terminal["port"], cell["connections"])
                    bel = cell["attributes"]["NEXTPNR_BEL"].split(".")
                    self.assertEqual(terminal["loc"], [int(bel[-3]), int(bel[-2])])
                if segment["type"] == "routing":
                    source, sink = segment["from"], segment["to"]
                    self.assertEqual(cells[source["cell"]]["connections"][source["port"]],
                                     cells[sink["cell"]]["connections"][sink["port"]])
                elif segment["type"] == "logic":
                    self.assertEqual(segment["from"]["cell"], segment["to"]["cell"])
            if endpoint != "launch":
                self.assertTrue(any(segment["type"] == "logic" and
                                    segment["from"]["cell"] == endpoint + "$logic" for segment in path), report)
        self.assertEqual(len(endpoints), len(set(endpoints)), timing)
        return endpoints

    def test_opt_in_keeps_legacy_prefix_and_reports_distinct_tied_endpoints(self):
        default = self.run_design()
        selected = self.run_design(count=3)
        all_paths = self.run_design(count=256)
        maximum_paths = self.run_design(count=16384)
        for code, log, outputs in (default, selected, all_paths, maximum_paths):
            self.assertEqual(code, 0, log)
            self.assertEqual(set(outputs), {"output.json", "timing.json"}, log)
            self.assertNotIn("Routing...", log)
            self.assertNotIn("Running the GPU router", log)
        base = json.loads(default[2]["timing.json"])
        for answer, count, expected in ((selected, 3, 3), (all_paths, 256, 4), (maximum_paths, 16384, 4)):
            timing = json.loads(answer[2]["timing.json"])
            self.assertEqual(timing["timing_report_paths"], count)
            self.assertEqual(timing["critical_paths"][:len(base["critical_paths"])], base["critical_paths"])
            self.assertEqual(timing["fmax"], base["fmax"])
            self.assertEqual(answer[2]["output.json"], default[2]["output.json"])
            placed = json.loads(answer[2]["output.json"])
            endpoints = self.assert_native_paths(timing, placed)
            self.assertEqual(len(endpoints), expected, timing)
            tied = [name for name in endpoints[1:] if name != "launch"]
            self.assertEqual(tied, sorted(tied))
            self.assertIn("additional registered setup endpoint paths", answer[1])
            self.assertTrue(all(not net.get("attributes", {}).get("ROUTING")
                                for net in placed["modules"]["top"]["netnames"].values()))
        self.assertEqual(set(self.assert_native_paths(json.loads(all_paths[2]["timing.json"]),
                                                     json.loads(all_paths[2]["output.json"]))),
                         {"launch", "capture_alpha", "capture_beta", "capture_zeta"})


if __name__ == "__main__":
    unittest.main()
