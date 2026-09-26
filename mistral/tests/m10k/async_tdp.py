#!/usr/bin/env python3
"""Route a true-dual-port M10K with asynchronous outputs on both ports."""
import argparse
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
CELL = "MISTRAL_M10K"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    qsf = output / "pins.qsf"
    qsf.write_text(args.qsf.read_text().replace("-to LED[0]", "-to LED"))

    source = Path(__file__).with_suffix(".v").resolve()
    synth = output / "synth.ys"
    synth.write_text(
        f"read_verilog {source}\n"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        f"select -assert-count 1 t:MISTRAL_M10K_TDP\n"
        f"write_json {output / 'synth.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(synth)], output / "synth.log")
    design = json.loads((output / "synth.json").read_text())
    name, cell = next((n, c) for n, c in design["modules"]["top"]["cells"].items()
                       if c["type"] == "MISTRAL_M10K_TDP")
    assert int(cell["parameters"]["CFG_ABITS"], 2) == 10
    assert int(cell["parameters"]["CFG_DBITS"], 2) == 10
    assert int(cell["parameters"]["CFG_ASYNC_READ"], 2) == 1
    for port in ("CLK1", "CLK2", "A1ADDR", "B1ADDR", "A1DATA", "B1DATA",
                 "A1EN", "B1EN", "A1WE", "B1WE", "A1Q", "B1Q"):
        assert cell["connections"].get(port), port

    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(qsf), "--sdc", str(args.sdc.resolve()),
         "--json", str(output / "synth.json"), "--compress-rbf",
         "--rbf", str(output / "top.rbf"), "--write", str(output / "routed.json"),
         "--report", str(output / "timing.json"), "--detailed-timing-report"], output / "route.log")
    assert (output / "top.rbf").stat().st_size > 0
    report = json.loads((output / "timing.json").read_text())
    assert report["utilization"][CELL]["used"] == 1
    assert report["utilization"]["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    assert report["utilization"]["altera_pll"]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"]
                                   for clock in report["fmax"].values())
    assert report["fmax"].get("clock_b", {}).get("constraint") == 25
    arcs = [arc for path in report["critical_paths"] for arc in path["path"]]
    for side in ("A", "B"):
        assert any(arc["type"] == "logic" and arc["delay"] == 1.5 and
                   arc["from"]["cell"] == name and
                   arc["from"]["port"].startswith(side + "1ADDR") and
                   arc["to"]["cell"] == name and
                   arc["to"]["port"].startswith(side + "1Q") for arc in arcs), \
            f"missing asynchronous {side}-port M10K timing arc"
        assert not any(arc["type"] == "clk-to-q" and arc["from"]["cell"] == name and
                       arc["from"]["port"].startswith(side + "1Q") for arc in arcs), \
            f"asynchronous {side}-port output still has a clock-to-Q arc"

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(output / "top.rbf"),
         str(output / "top.bt")], output / "decomp.log")
    routed = json.loads((output / "routed.json").read_text())["modules"]["top"]["cells"][name]
    assert routed["type"] == CELL and int(routed["parameters"]["CFG_TDP"], 2) == 1
    _, x, y, _ = routed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    bitstream = (output / "top.bt").read_text()
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             bitstream, re.MULTILINE))
    for field in ("TRUE_DUAL_PORT", "A_DATA_FLOW_THRU", "B_DATA_FLOW_THRU",
                  "TOP_CLK_SEL",
                  "TOP_CORECLK_SEL", "TOP_INCLK_SEL", "BOT_CLK_SEL",
                  "BOT_CORECLK_SEL", "BOT_INCLK_SEL", "BOT_1_CORECLK_SEL",
                  "BOT_1_INCLK_SEL", "BOT_1_OUTCLK_SEL"):
        assert fields.get(field, "0") == "1", (field, fields.get(field), fields)
    assert fields.get("A_OUTPUT_SEL", "async") == "async", fields
    assert fields.get("B_OUTPUT_SEL", "async") == "async", fields
    for field in ("A_DATA_WIDTH", "B_DATA_WIDTH"):
        assert fields[field] == "10"
    for pin in ("CLKIN.0", "CLKIN.1", "WREN.0", "WREN.1", "ENABLE.0", "ENABLE.1"):
        assert re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$",
                         bitstream, re.MULTILINE), pin
    print("PASS: asynchronous TDP M10K, both address-to-Q arcs, selectors, RBF and 50 MHz timing")


if __name__ == "__main__":
    main()
