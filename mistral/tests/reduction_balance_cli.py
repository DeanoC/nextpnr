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


def add_eleven_cone(fixture):
    top = fixture["modules"]["top"]
    inputs = list(range(32, 43))
    top["cells"].update({
        "eleven_leaf": lut(4, 1 << 15, inputs[:4], 43),
        "eleven_inner": lut(5, 1 << 31, inputs[4:8] + [43], 44),
        "eleven_root": lut(4, 1 << 15, inputs[8:] + [44], 45),
    })
    top["ports"].update({"renamed_inputs": {"direction": "input", "bits": inputs},
                         "renamed_output": {"direction": "output", "bits": [45]}})
    top["netnames"].update({"renamed_inputs": {"bits": inputs}, "renamed_output": {"bits": [45]}})


def add_twentyfour_cone(fixture):
    top = fixture["modules"]["top"]
    literals = list(range(64, 88))
    clock, data = 152, list(range(128, 152))
    ff_inputs = ("CLK", "ENA", "ACLR", "SCLR", "SLOAD", "SDATA", "DATAIN")

    def ff(q, datain, ena="1"):
        return {"type": "MISTRAL_FF", "parameters": {},
                "connections": {"CLK": [clock], "ENA": [ena], "ACLR": ["1"], "SCLR": ["0"],
                                "SLOAD": ["0"], "SDATA": ["0"], "DATAIN": [datain], "Q": [q]},
                "port_directions": {**{p: "input" for p in ff_inputs}, "Q": "output"}}

    required = [i % 3 != 1 for i in range(24)]
    branches, branch_values = [], []
    for branch in range(3):
        first_positive, second_positive = branch == 1, branch != 1
        first_row = sum(int(required[8 * branch + i]) << i for i in range(4))
        first_mask = (1 << first_row) ^ (0 if first_positive else 0xffff)
        second_row = sum(int(required[8 * branch + 4 + i]) << i for i in range(4))
        second_row |= int(first_positive) << 4
        second_mask = (1 << second_row) ^ (0 if second_positive else 0xffffffff)
        first_q, second_q = 88 + 2 * branch, 89 + 2 * branch
        top["cells"][f"wide_branch_{branch}_first"] = lut(4, first_mask, literals[8 * branch:8 * branch + 4], first_q)
        top["cells"][f"wide_branch_{branch}_middle"] = lut(5, second_mask,
                                                              literals[8 * branch + 4:8 * branch + 8] + [first_q], second_q)
        branches.append(second_q)
        branch_values.append(second_positive)
    root_row = sum(int(v) << i for i, v in enumerate(branch_values))
    top["cells"]["wide_root"] = lut(3, 1 << root_row, branches, 94)
    for i, signal in enumerate(literals):
        top["cells"][f"wide_source_{i:02d}"] = ff(signal, data[i])
    for i in range(9):
        top["cells"][f"wide_sink_{i}"] = ff(160 + i, 160 + i, 94)
    top["ports"].update({"wide_data": {"direction": "input", "bits": data},
                         "wide_clock": {"direction": "input", "bits": [clock]},
                         "wide_public_result": {"direction": "output", "bits": [94]},
                         "wide_public_literal": {"direction": "output", "bits": [literals[7]]}})
    top["netnames"].update({"wide_data": {"bits": data}, "wide_clock": {"bits": [clock]},
                           "wide_literals": {"bits": literals}, "wide_result": {"bits": [94]}})


def named_connections(top, cell_name):
    drivers = {bit: (name, port, index)
               for name, cell in top["cells"].items()
               for port, bits in cell["connections"].items() if cell["port_directions"][port] == "output"
               for index, bit in enumerate(bits)}
    drivers.update({bit: ("input", name, index)
                    for name, port in top["ports"].items() if port["direction"] == "input"
                    for index, bit in enumerate(port["bits"])})
    return {port: [drivers.get(bit, ("constant", bit)) for bit in bits]
            for port, bits in top["cells"][cell_name]["connections"].items()}


def public_output_cone(top, port_name, root_name):
    """Describe loaded IO wrappers, stopping at the named preserved Q driver."""
    drivers = {bit: (name, port, index)
               for name, cell in top["cells"].items()
               for port, bits in cell["connections"].items() if cell["port_directions"][port] == "output"
               for index, bit in enumerate(bits)}
    found_root = set()

    def descend(bit, active):
        if bit in ("0", "1"):
            return ("constant", bit)
        if bit in active or bit not in drivers:
            raise AssertionError("cyclic or undriven public output")
        name, port, index = drivers[bit]
        if name == root_name:
            if port != "Q":
                raise AssertionError("public output does not use retained Q")
            found_root.add(name)
            return ("retained_root", name, port, index)
        cell = top["cells"][name]
        inputs = tuple((pin, tuple(descend(value, active | {bit}) for value in bits))
                       for pin, bits in sorted(cell["connections"].items())
                       if cell["port_directions"][pin] == "input")
        return (name, port, index, cell["type"], tuple(sorted(cell["parameters"].items())), inputs)

    public_port = top["ports"][port_name]
    if public_port["direction"] != "output":
        raise AssertionError("public result is not an output")
    boundary = top["cells"].get(port_name)
    if boundary is not None and boundary["type"] == "$nextpnr_obuf":
        # The actual no-pack export associates its pseudo IO wrapper by the
        # public port name. It exports only I; its separate pad bit has no
        # ordinary cell driver and must not be treated as an internal net.
        if (boundary["port_directions"] != {"I": "input"} or
                set(boundary["connections"]) != {"I"} or
                len(boundary["connections"]["I"]) != 1 or len(public_port["bits"]) != 1):
            raise AssertionError("unexpected public output wrapper shape")
        result = ("named_obuf", port_name, boundary["type"],
                  tuple(sorted(boundary["parameters"].items())),
                  tuple(sorted(boundary.get("attributes", {}).items())),
                  descend(boundary["connections"]["I"][0], set()))
    else:
        result = tuple(descend(bit, set()) for bit in public_port["bits"])
    if found_root != {root_name}:
        raise AssertionError("retained root is absent from public output cone")
    return result


def cube_literals(top, root):
    """Symbolically flatten unique LUT assignments, enumerating at most 64 rows."""
    drivers = {bits[0]: name for name, cell in top["cells"].items()
               for port, bits in cell["connections"].items() if port == "Q" and bits}
    literals, visited = {}, set()

    def descend(bit, required):
        name = drivers.get(bit)
        cell = top["cells"].get(name, {})
        if not cell.get("type", "").startswith("MISTRAL_ALUT"):
            if name in literals:
                raise AssertionError("repeated literal")
            literals[name] = required
            return
        if name in visited:
            raise AssertionError("reconvergent or cyclic cube")
        visited.add(name)
        width = int(cell["type"].removeprefix("MISTRAL_ALUT"))
        mask = int(cell["parameters"]["LUT"], 2)
        matching = [row for row in range(1 << width) if bool((mask >> row) & 1) == required]
        if len(matching) != 1:
            raise AssertionError("not a unique cube assignment")
        for i, pin in enumerate("ABCDEF"[:width]):
            descend(cell["connections"][pin][0], bool((matching[0] >> i) & 1))

    descend(top["cells"][root]["connections"]["Q"][0], True)
    return literals


class ReductionBalanceCliTest(unittest.TestCase):
    def run_design(self, exposed_child=False, root="root", output_attribute=None, root_table=None,
                   clock_net=None, step=None, mode=None, eleven=False, twentyfour=False, mutate=None):
        with tempfile.TemporaryDirectory(prefix="reduction-balance-") as directory:
            folder = Path(directory)
            fixture = design(exposed_child)
            if eleven:
                add_eleven_cone(fixture)
            if twentyfour:
                add_twentyfour_cone(fixture)
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
            if mutate is not None:
                mutate(fixture["modules"]["top"])
            (folder / "input.json").write_text(json.dumps(fixture))
            command = [BINARY, "--device", "5CSEBA6U23I7", "--json", str(folder / "input.json"),
                       "--no-pack", "--no-place", "--no-route", "--write", str(folder / "output.json")]
            if root is not None:
                for selected in ([root] if isinstance(root, str) else root):
                    command += ["--balance-reduction-root", selected]
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

    def test_eleven_literal_cone_and_repeatable_root_selection(self):
        for roots in (["eleven_root"], ["root", "eleven_root"], ["eleven_root", "root"]):
            with self.subTest(roots=roots):
                code, log, output = self.run_design(root=roots, eleven=True)
                self.assertEqual(code, 0, log)
                cells = output["modules"]["top"]["cells"]
                self.assertEqual(cells["eleven_root"]["type"], "MISTRAL_ALUT2")
                self.assertEqual(cells["eleven_inner"]["type"], "MISTRAL_ALUT6")
                self.assertEqual(cells["eleven_leaf"]["type"], "MISTRAL_ALUT5")
                self.assertEqual(int(cells["eleven_root"]["parameters"]["LUT"], 2), 0x8)
                self.assertEqual(cells["root"]["type"],
                                 "MISTRAL_ALUT3" if "root" in roots else "MISTRAL_ALUT5")

    def test_rejects_intermediate_output_with_an_external_user(self):
        code, log, output = self.run_design(exposed_child=True)
        self.assertNotEqual(code, 0, log)
        self.assertIn("not a safe four-LUT", log)
        self.assertIsNone(output)

    def test_rejects_absent_root(self):
        code, log, output = self.run_design(root="absent")
        self.assertNotEqual(code, 0, log)
        self.assertIsNone(output)

    def test_repeated_selection_failure_writes_no_partial_output(self):
        for roots in (["root", "absent"], ["absent", "root"]):
            with self.subTest(roots=roots):
                code, log, output = self.run_design(root=roots)
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

    def test_twentyfour_cube_retires_two_private_nodes_and_preserves_external_ffs(self):
        code, log, output = self.run_design(root="wide_root", twentyfour=True)
        self.assertEqual(code, 0, log)
        top = output["modules"]["top"]
        cells = top["cells"]
        self.assertEqual(cells["wide_root"]["type"], "MISTRAL_ALUT4")
        self.assertEqual(int(cells["wide_root"]["parameters"]["LUT"], 2), 0x8000)
        for name in ("wide_branch_0_first", "wide_branch_0_middle", "wide_branch_1_first", "wide_branch_1_middle"):
            self.assertEqual(cells[name]["type"], "MISTRAL_ALUT6")
        for name in ("wide_branch_2_first", "wide_branch_2_middle"):
            self.assertNotIn(name, cells)
        self.assertEqual(cube_literals(top, "wide_root"), {f"wide_source_{i:02d}": i % 3 != 1 for i in range(24)})
        # The real loader lowers literal constants into named driver cells.
        # Compare two loaded copies of the same fixture, with exact named
        # connections, rather than comparing lowered bytes to source JSON.
        disabled_code, disabled_log, disabled_output = self.run_design(root=None, twentyfour=True)
        self.assertEqual(disabled_code, 0, disabled_log)
        source = disabled_output["modules"]["top"]
        for name, cell in source["cells"].items():
            if cell["type"] == "MISTRAL_FF":
                self.assertEqual(cells[name]["type"], cell["type"])
                self.assertEqual(cells[name]["parameters"], cell["parameters"])
                self.assertEqual(named_connections(top, name), named_connections(source, name))
        for i in range(9):
            self.assertEqual(named_connections(top, f"wide_sink_{i}")["ENA"], [("wide_root", "Q", 0)])
        for public_port, driver in (("wide_public_result", "wide_root"), ("wide_public_literal", "wide_source_07")):
            self.assertEqual(public_output_cone(top, public_port, driver),
                             public_output_cone(source, public_port, driver))
            # Names are compared through each export's actual Q bits; numeric
            # bit IDs may be reassigned when two private nodes are retired.
            def q_names(module):
                q = module["cells"][driver]["connections"]["Q"]
                return sorted(name for name, net in module["netnames"].items() if net["bits"] == q)
            self.assertTrue(q_names(source))
            self.assertEqual(q_names(top), q_names(source))

    def test_repeatable_eleven_and_twentyfour_selection(self):
        for roots in (["eleven_root", "wide_root"], ["wide_root", "eleven_root"]):
            with self.subTest(roots=roots):
                code, log, output = self.run_design(root=roots, eleven=True, twentyfour=True)
                self.assertEqual(code, 0, log)
                cells = output["modules"]["top"]["cells"]
                self.assertEqual(cells["eleven_root"]["type"], "MISTRAL_ALUT2")
                self.assertEqual(cells["wide_root"]["type"], "MISTRAL_ALUT4")
                self.assertEqual(cells["root"]["type"], "MISTRAL_ALUT5")
                self.assertEqual(cube_literals(output["modules"]["top"], "wide_root"),
                                 {f"wide_source_{i:02d}": i % 3 != 1 for i in range(24)})

    def test_twentyfour_disabled_keeps_all_original_nodes(self):
        code, log, output = self.run_design(root=None, twentyfour=True)
        self.assertEqual(code, 0, log)
        cells = output["modules"]["top"]["cells"]
        for branch in range(3):
            self.assertEqual(cells[f"wide_branch_{branch}_first"]["type"], "MISTRAL_ALUT4")
            self.assertEqual(cells[f"wide_branch_{branch}_middle"]["type"], "MISTRAL_ALUT5")
        self.assertEqual(cells["wide_root"]["type"], "MISTRAL_ALUT3")

    def test_twentyfour_rejects_unsafe_structure_without_output(self):
        def repeated(top):
            top["cells"]["wide_branch_0_first"]["connections"]["A"] = [65]

        def reconvergent(top):
            top["cells"]["wide_root"]["connections"]["B"] = list(top["cells"]["wide_root"]["connections"]["A"])

        def cycle(top):
            top["cells"]["wide_branch_0_first"]["connections"]["A"] = [94]

        def noncube(top):
            top["cells"]["wide_root"]["parameters"]["LUT"] = "00000011"

        def sideuser(top):
            top["cells"]["wide_sink_0"]["connections"]["DATAIN"] = [88]

        def boundary(top):
            top["ports"]["private_result"] = {"direction": "output", "bits": [88]}

        def protected(top):
            top["cells"]["wide_branch_0_first"]["attributes"] = {"dont_touch": 1}

        for mutate in (repeated, reconvergent, cycle, noncube, sideuser, boundary, protected):
            with self.subTest(mutation=mutate.__name__):
                code, log, output = self.run_design(root="wide_root", twentyfour=True, mutate=mutate)
                self.assertNotEqual(code, 0, log)
                self.assertIn("not a safe four-LUT", log)
                self.assertIsNone(output)


if __name__ == "__main__":
    unittest.main()
