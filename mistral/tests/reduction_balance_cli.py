#!/usr/bin/env python3
"""Exercise the opt-in reduction rewrite through the real JSON loader."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


def lut(width, mask, inputs, output):
    names = "ABCDEF"[:width]
    return {"type": f"MISTRAL_ALUT{width}",
            "parameters": {"LUT": format(mask, f"0{1 << width}b")},
            "connections": {**{p: [v] for p, v in zip(names, inputs)}, "Q": [output]},
            "port_directions": {**{p: "input" for p in names}, "Q": "output"}}


def design(exposed_child=False):
    # root = !(e0|e1|e2|e3) & inner, with inner depending on two NOR4s.
    inputs = list(range(2, 18))
    cells = {
        "root": lut(5, 1 << 16, inputs[:4] + [20], 21),
        "inner": lut(6, 1 << 48, inputs[4:8] + [18, 19], 20),
        "leaf_a": lut(4, 1, inputs[8:12], 18),
        "leaf_b": lut(4, 1, inputs[12:16], 19),
    }
    ports = {"errors": {"direction": "input", "bits": inputs},
             "out": {"direction": "output", "bits": [21]}}
    if exposed_child:
        ports["shared"] = {"direction": "output", "bits": [18]}
    return {"modules": {"top": {"attributes": {"top": 1}, "ports": ports, "cells": cells,
                                "netnames": {"errors": {"bits": inputs}, "out": {"bits": [21]}}}}}


class ReductionBalanceCliTest(unittest.TestCase):
    def run_design(self, exposed_child=False, root="root", output_attribute=None, root_table=None,
                   clock_net=None, step=None, mode=None):
        with tempfile.TemporaryDirectory(prefix="reduction-balance-") as directory:
            folder = Path(directory)
            fixture = design(exposed_child)
            if step is not None:
                fixture["modules"]["top"]["attributes"]["step"] = step
            if output_attribute is not None:
                fixture["modules"]["top"]["netnames"]["out"]["attributes"] = {output_attribute: 1}
            if root_table is not None:
                fixture["modules"]["top"]["cells"]["root"]["parameters"]["LUT"] = root_table
            if clock_net is not None:
                fixture["modules"]["top"]["cells"]["clock_sink"] = {
                    "type": "MISTRAL_FF", "parameters": {},
                    "connections": {"CLK": [clock_net], "ENA": ["1"], "ACLR": ["1"],
                                    "SCLR": ["0"], "SLOAD": ["0"], "SDATA": ["0"],
                                    "DATAIN": ["0"], "Q": [22]},
                    "port_directions": {**{p: "input" for p in
                                           ("CLK", "ENA", "ACLR", "SCLR", "SLOAD", "SDATA", "DATAIN")},
                                        "Q": "output"}}
            (folder / "input.json").write_text(json.dumps(fixture))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(folder / "input.json"),
                       "--no-pack", "--no-place", "--no-route", "--write", str(folder / "output.json")]
            if root is not None:
                command += ["--balance-reduction-root", root]
            if mode is not None:
                command += [mode]
                if mode == "--fes-cart":
                    command += [str(folder / "absent-cart.json")]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=30)
            output = json.loads((folder / "output.json").read_text()) if (folder / "output.json").exists() else None
            return result.returncode, result.stdout, output

    def test_rewrites_only_the_selected_four_luts(self):
        code, log, output = self.run_design()
        self.assertEqual(code, 0, log)
        cells = output["modules"]["top"]["cells"]
        for name, kind in {"root": "MISTRAL_ALUT3", "inner": "MISTRAL_ALUT6",
                           "leaf_a": "MISTRAL_ALUT6", "leaf_b": "MISTRAL_ALUT4"}.items():
            self.assertEqual(cells[name]["type"], kind)
        self.assertEqual(int(cells["root"]["parameters"]["LUT"], 2), 0x80)

    def test_rejects_intermediate_output_with_an_external_user(self):
        code, log, output = self.run_design(exposed_child=True)
        self.assertNotEqual(code, 0, log)
        self.assertIn("not a safe four-LUT", log)
        self.assertIsNone(output)

    def test_rejects_absent_root(self):
        code, log, output = self.run_design(root="absent")
        self.assertNotEqual(code, 0, log)
        self.assertIsNone(output)

    def test_rejects_protected_root_output(self):
        for attribute in ("keep", "dont_touch"):
            with self.subTest(attribute=attribute):
                code, log, output = self.run_design(output_attribute=attribute)
                self.assertNotEqual(code, 0, log)
                self.assertIn("not a safe four-LUT", log)
                self.assertIsNone(output)

    def test_rejects_undefined_or_malformed_truth_tables(self):
        valid = format(1 << 16, "032b")
        for table in ("x" + valid[1:], "z" + valid[1:], "invalid lut", valid[-17:], "0" + valid):
            with self.subTest(table=table):
                code, log, output = self.run_design(root_table=table)
                self.assertNotEqual(code, 0, log)
                self.assertIn("not a safe four-LUT", log)
                self.assertIsNone(output)

    def test_rejects_unconstrained_clock_consumers(self):
        for net in (2, 21):
            with self.subTest(net=net):
                code, log, output = self.run_design(clock_net=net)
                self.assertNotEqual(code, 0, log)
                self.assertIn("not a safe four-LUT", log)
                self.assertIsNone(output)

    def test_rejects_saved_implementation_stages(self):
        for step in ("pack", "place", "route"):
            with self.subTest(step=step):
                code, log, output = self.run_design(step=step)
                self.assertNotEqual(code, 0, log)
                self.assertIn("requires an unpacked, ordinary full design", log)
                self.assertIsNone(output)

    def test_rejects_scaffold_and_cart_modes(self):
        for mode in ("--fes-scaffold", "--fes-cart"):
            with self.subTest(mode=mode):
                code, log, output = self.run_design(mode=mode)
                self.assertNotEqual(code, 0, log)
                self.assertIn("requires an unpacked, ordinary full design", log)
                self.assertIsNone(output)

    def test_disabled_mode_keeps_original_lut_types(self):
        code, log, output = self.run_design(root=None)
        self.assertEqual(code, 0, log)
        cells = output["modules"]["top"]["cells"]
        for name, kind in {"root": "MISTRAL_ALUT5", "inner": "MISTRAL_ALUT6",
                           "leaf_a": "MISTRAL_ALUT4", "leaf_b": "MISTRAL_ALUT4"}.items():
            self.assertEqual(cells[name]["type"], kind)


if __name__ == "__main__":
    unittest.main()
