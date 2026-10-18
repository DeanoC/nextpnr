#!/usr/bin/env python3
"""Route the misteross 060 fixture and inspect the actual RBF configuration.

Requires synth.json regenerated with locked Yosys, plus the board QSF/SDC.
Variants exercise explicit signedness, constant 1 and folded input inversion.
No hardware is programmed and the input fixture is never modified.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(command, log):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "mistral-cv", "fixture", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    design = json.loads(args.fixture.read_text())
    cells = design["modules"]["top"]["cells"]
    mul_names = [n for n, c in cells.items() if c["type"] == "MISTRAL_MUL9X9"]
    assert len(mul_names) == 1
    mul_name = mul_names[0]
    assert cells[mul_name]["connections"]["A"][8] == "0"
    assert cells[mul_name]["connections"]["B"][8] == "0"

    for variant in ("060", "unsigned-high", "mixed-inverted"):
        out = args.output.resolve() / variant
        out.mkdir(parents=True, exist_ok=True)
        test = copy.deepcopy(design)
        test_cells = test["modules"]["top"]["cells"]
        mul = test_cells[mul_name]
        signs = (1, 1)
        masks = (0x100, 0x100)
        if variant != "060":
            signs = (0, int(variant == "mixed-inverted"))
            mul["parameters"].update(A_SIGNED=str(signs[0]), B_SIGNED=str(signs[1]))
            mul["connections"]["A"][8] = "1"
            mul["connections"]["B"][8] = "1"
            masks = (0, 0)
        if variant == "mixed-inverted":
            bits = [b for c in test_cells.values() for p in c["connections"].values()
                    for b in p if isinstance(b, int)]
            new_bit = max(bits) + 1
            test_cells["dsp_input_inverter"] = {
                "type": "MISTRAL_NOT", "parameters": {}, "attributes": {},
                "port_directions": {"A": "input", "Q": "output"},
                "connections": {"A": [mul["connections"]["A"][0]], "Q": [new_bit]},
            }
            mul["connections"]["A"][0] = new_bit
            masks = (1, 0)
        (out / "synth.json").write_text(json.dumps(test))
        run([str(args.nextpnr.resolve()), "--json", str(out / "synth.json"),
             "--device", "5CSEBA6U23I7", "--qsf", str(args.qsf.resolve()),
             "--sdc", str(args.sdc.resolve()), "--freq", "50", "--compress-rbf",
             "--rbf", str(out / "top.rbf"), "--write", str(out / "routed.json"),
             "--report", str(out / "timing.json"), "--detailed-timing-report"], out / "route.log")
        report = json.loads((out / "timing.json").read_text())
        util = report["utilization"]
        # The BEL class exposes three logical lanes for each of the 112
        # physical DSP blocks. Physical capacity is 112 blocks; the report is
        # expressed in placeable lane BELs.
        assert util["MISTRAL_MUL9X9"] == {"used": 1, "available": 336}
        assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        assert util["MISTRAL_M10K"]["used"] == 0
        assert util["altera_pll"]["used"] == 0
        clock = report["fmax"]["product.FPGA_CLK1_50"]
        assert clock["constraint"] == 50 and clock["achieved"] >= 50
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
        bt = (out / "top.bt").read_text()
        sites = re.findall(r"^s (DSP\.\d+\.\d+):MODE M9X9$", bt, re.M)
        assert len(sites) == 1, sites
        site = sites[0]
        settings = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$", bt, re.M))
        # Quartus sets both multipliers' sign bits for a signed operand.
        for key, expected in zip(("AX_SIGNED", "BX_SIGNED", "AY_SIGNED", "BY_SIGNED"),
                                 (signs[0], signs[0], signs[1], signs[1])):
            assert int(settings.get(key, "0")) == expected, (variant, key, settings)
        # A signed lane-0 operand is sign-extended into the upper slice
        # (groups 1 and 3). The fixture's sign bits are constants: a 0 leaves
        # the slice unused (inverted), a 1 drives it high (not inverted).
        sign_values = (int(mul["connections"]["A"][8]), int(mul["connections"]["B"][8]))
        ext_masks = {}
        for group, is_signed, sign in zip((1, 3), signs, sign_values):
            if is_signed and sign:
                ext_masks[group] = 0
        for group in range(12):
            expected = masks[0] if group == 0 else masks[1] if group == 2 else ext_masks.get(group, 0x1ff)
            assert int(settings.get(f"DATA_INV.{group}", "0"), 16) == expected, (variant, group, settings)
        # Register bypass is the Mistral default; decomp emits nondefaults only.
        for key in ("INREG_CTRL_AX", "INREG_CTRL_AY", "OREG_CTRL"):
            assert settings.get(key, "BYPASS") == "BYPASS"
        assert f"{site}.0:DATAIN.0" in bt and f"{site}.2:DATAIN.7" in bt
        assert f"{site}:RESULT.15" in bt
        rbf = (out / "top.rbf").read_bytes()
        assert rbf
        print(variant, "PASS", clock, hashlib.sha256(rbf).hexdigest(), flush=True)


if __name__ == "__main__":
    main()
