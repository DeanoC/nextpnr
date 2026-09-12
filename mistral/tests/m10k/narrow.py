#!/usr/bin/env python3
"""Route equal-width narrow true-dual-port Cyclone V M10Ks.

The direct primitive is intentional: the locked Yosys M10K TDP rules cover
10- and 20-bit ports, while Cyclone V also has native 1-, 2- and 5-bit
equal-width true-dual-port geometries.  This test keeps the nextpnr packing
contract visible until Yosys grows matching inference rules.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
CELL = "MISTRAL_M10K_TDP"
PHYSICAL_CELL = "MISTRAL_M10K"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--case", action="append",
                        help="Run only wWIDTH cases (repeatable)")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_suffix(".v").resolve()

    for width, abits in ((1, 13), (2, 12), (5, 11)):
        label = f"w{width}"
        if args.case and label not in args.case:
            continue
        case = output / label
        case.mkdir(parents=True, exist_ok=True)
        script = case / "synth.ys"
        script.write_text(
            f"read_verilog {source}\n"
            f"chparam -set WIDTH {width} -set ABITS {abits} top\n"
            "synth_intel_alm -nolutram -nodsp -top top\n"
            f"select -assert-count 1 t:{CELL}\n"
            f"write_json {case / 'synth.json'}\n"
        )
        run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)],
            case / "synth.log")

        design = json.loads((case / "synth.json").read_text())
        cells = design["modules"]["top"]["cells"]
        name, cell = next((n, c) for n, c in cells.items()
                          if c["type"] == CELL)
        assert int(cell["parameters"]["CFG_ABITS"], 2) == abits
        assert int(cell["parameters"]["CFG_DBITS"], 2) == width
        assert len(cell["connections"]["A1ADDR"]) == abits
        assert len(cell["connections"]["B1ADDR"]) == abits
        assert len(cell["connections"]["A1DATA"]) == width
        assert len(cell["connections"]["B1DATA"]) == width

        run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
             "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
             "--json", str(case / "synth.json"), "--compress-rbf",
             "--rbf", str(case / "top.rbf"), "--write", str(case / "routed.json"),
             "--report", str(case / "timing.json")], case / "route.log")

        assert (case / "top.rbf").stat().st_size > 0
        report = json.loads((case / "timing.json").read_text())
        assert report["utilization"][PHYSICAL_CELL]["used"] == 1
        for resource in ("MISTRAL_MLAB", "MISTRAL_MUL9X9", "MISTRAL_MUL18X18",
                         "MISTRAL_MUL27X27", "MISTRAL_MUL18X19",
                         "MISTRAL_MUL18X19_COMBINED", "altera_pll"):
            assert report["utilization"].get(resource, {}).get("used", 0) == 0, (label, resource)
        assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                       for clock in report["fmax"].values())

        run([str(args.mistral_cv.resolve()), "decomp", DEVICE,
             str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        packed = json.loads((case / "routed.json").read_text())[
            "modules"]["top"]["cells"][name]
        assert packed["type"] == PHYSICAL_CELL
        assert int(packed["parameters"]["CFG_TDP"], 2) == 1
        _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
        site = f"M10K.{int(x):03d}.{int(y):03d}"
        fields = dict(re.findall(
            r"^s " + re.escape(site) + r":(\S+) (\S+)$",
            (case / "top.bt").read_text(), re.MULTILINE))

        for field in ("TRUE_DUAL_PORT", "TOP_CLK_SEL", "TOP_CORECLK_SEL",
                      "TOP_INCLK_SEL", "BOT_CLK_SEL", "BOT_CORECLK_SEL",
                      "BOT_INCLK_SEL", "BOT_1_CORECLK_SEL",
                      "BOT_1_INCLK_SEL", "BOT_1_OUTCLK_SEL",
                      "A_DATA_FLOW_THRU", "B_DATA_FLOW_THRU"):
            assert fields.get(field, "0") == "1", (label, field, fields)
        assert fields.get("A_DATA_WIDTH", "1") == str(width), fields
        assert fields.get("B_DATA_WIDTH", "1") == str(width), fields

        bitstream = (case / "top.bt").read_text()
        for pin in ("CLKIN.0", "CLKIN.1", "WREN.0", "WREN.1",
                    "ENABLE.0", "ENABLE.1"):
            assert re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$",
                             bitstream, re.MULTILINE), (label, pin)
        print(f"PASS: {label}: one narrow TDP M10K, selectors, RBF and 50 MHz",
              flush=True)


if __name__ == "__main__":
    main()
