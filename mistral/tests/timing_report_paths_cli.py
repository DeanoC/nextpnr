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

    def test_routed_report_replay_refreshes_the_actual_final_graph(self):
        with tempfile.TemporaryDirectory(prefix="timing-report-paths-routed-cli-") as directory:
            root = Path(directory)
            module = placed_fixture()
            # Leave the launch FF's ALM free for the backend's real DATAIN
            # route-through. Constants occupy an otherwise empty ALM instead.
            for name, slot in (("$PACKER_VCC_DRV", 54), ("$PACKER_GND_DRV", 55)):
                module["cells"][name]["attributes"]["NEXTPNR_BEL"] = f"MISTRAL_COMB.24.20.{slot}"
            (root / "design.json").write_text(json.dumps({"modules": {"top": module}}))
            (root / "clock.sdc").write_text("create_clock -period 10 [get_nets clock]\n")
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("NEXTPNR_MISTRAL_")}
            reports = []
            graphs = []
            logs = []
            for replay in (False, True):
                stem = "replayed" if replay else "routed"
                source = root / ("routed.json" if replay else "design.json")
                command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(source),
                           "--no-pack", "--no-place", "--sdc", str(root / "clock.sdc"),
                           "--router", "router1", "--timing-allow-fail",
                           "--write", str(root / f"{stem}.json"),
                           "--report", str(root / f"{stem}-timing.json"),
                           "--timing-report-paths", "256", "--detailed-timing-report"]
                if replay:
                    command.append("--no-route")
                result = subprocess.run(command, cwd=root, env=environment, timeout=90,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertEqual(result.returncode, 0, result.stdout)
                graph = json.loads((root / f"{stem}.json").read_text())
                timing = json.loads((root / f"{stem}-timing.json").read_text())
                top = graph["modules"]["top"]
                self.assertEqual(top["attributes"]["step"], "route")
                self.assertTrue(top["attributes"]["FES_LABSTATE_V1"])
                self.assertTrue(all(cell["attributes"].get("FES_PINMAP_V1")
                                    for cell in top["cells"].values()))
                self.assertTrue(any(net.get("attributes", {}).get("ROUTING")
                                    for net in top["netnames"].values()))
                self.assertEqual(timing["timing_report_paths"], 256)
                endpoints = self.assert_native_paths(timing, graph)
                self.assertEqual(set(endpoints), {"launch", "capture_alpha", "capture_beta", "capture_zeta"})
                self.assertTrue(timing["detailed_net_timings"])
                self.assertNotIn("Running predicted placed timing analysis", result.stdout)
                self.assertNotIn("Running the GPU router", result.stdout)
                self.assertNotIn("Running signoff timing analysis", result.stdout)
                if replay:
                    self.assertNotIn("Routing complete.", result.stdout)
                else:
                    self.assertIn("Routing complete.", result.stdout)
                reports.append(timing)
                graphs.append(graph)
                logs.append(result.stdout)
            # Re-importing the saved route must preserve its timing, rather
            # than publishing the empty TimingResult of a fresh Context.
            self.assertEqual(reports[0], reports[1])
            for log in logs:
                self.assertIn("Running final routed timing analysis for --report (no bitstream signoff).", log)
            before, after = [graph["modules"]["top"] for graph in graphs]
            self.assertEqual(set(before["cells"]), set(after["cells"]))
            self.assertEqual(set(before["netnames"]), set(after["netnames"]))
            # Serialized bit numbers are image-local. Compare their complete
            # named alias sets, while assert_native_paths checks each image's
            # actual raw source/sink connection and BEL independently.
            def connection_aliases(top):
                aliases = {}
                for name, net in top["netnames"].items():
                    self.assertEqual(len(net["bits"]), 1)
                    aliases.setdefault(net["bits"][0], []).append(name)
                result = {}
                for name, cell in top["cells"].items():
                    result[name] = {}
                    for port, bits in cell["connections"].items():
                        if not bits:
                            self.assertEqual(bits, [], (name, port))
                            continue
                        result[name][port] = [tuple(sorted(aliases[bit])) if isinstance(bit, int) else bit
                                              for bit in bits]
                return result
            self.assertEqual(connection_aliases(before), connection_aliases(after))
            for name, cell in before["cells"].items():
                other = after["cells"][name]
                for field in ("type", "parameters"):
                    self.assertEqual(cell[field], other[field])
                # JSON import omits disconnected ports exported as []. Only
                # these empty slots may disappear; every connected slot and
                # its direction must remain present and equal.
                connected = []
                for image in (cell, other):
                    connected.append({port for port, bits in image["connections"].items() if bits})
                    for port in set(image["port_directions"]) - connected[-1]:
                        self.assertIn(port, image["connections"])
                        self.assertEqual(image["connections"][port], [], (name, port))
                self.assertEqual(connected[0], connected[1])
                self.assertEqual({port: cell["port_directions"][port] for port in connected[0]},
                                 {port: other["port_directions"][port] for port in connected[1]})
                for field in ("NEXTPNR_BEL", "FES_PINMAP_V1"):
                    self.assertEqual(cell["attributes"][field], other["attributes"][field])
            for name, net in before["netnames"].items():
                self.assertEqual(net["attributes"], after["netnames"][name]["attributes"])


if __name__ == "__main__":
    unittest.main()
