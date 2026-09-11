#!/usr/bin/env python3
"""Check explicit registered-output modes on Cyclone V M10K cells.

The locked Yosys M10K model exposes a clocked B1DATA/A1Q output but does not
carry an output-register parameter.  These fixtures therefore add the future
JSON parameters at the test boundary.  The default remains the existing
unregistered physical mode; CFG_OUT_REG_A/B request the corresponding M10K
output register.
"""

import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
SDP = "MISTRAL_M10K"
TDP = "MISTRAL_M10K_TDP"
HPS = "cyclonev_hps_interface_mpu_general_purpose"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def synthesize(args, directory, source, cell_type, params):
    directory.mkdir(parents=True, exist_ok=True)
    script = directory / "synth.ys"
    changes = "".join(f"chparam -set {key} {value} top\n" for key, value in params.items())
    script.write_text(
        f"read_verilog {source}\n"
        f"{changes}"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        f"select -assert-count 1 t:{cell_type}\n"
        f"write_json {directory / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)], directory / "synth.log")
    design = json.loads((directory / "base.json").read_text())
    cells = design["modules"]["top"]["cells"]
    name, cell = next((n, c) for n, c in cells.items() if c["type"] == cell_type)
    return design, name, cell


def set_output_registers(design, name, a=False, b=False):
    cell = design["modules"]["top"]["cells"][name]
    cell["parameters"]["CFG_OUT_REG_A"] = int(a)
    cell["parameters"]["CFG_OUT_REG_B"] = int(b)


def route(args, directory, design, name, expected):
    directory.mkdir(parents=True, exist_ok=True)
    fixture = directory / "synth.json"
    fixture.write_text(json.dumps(design))
    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--compress-rbf", "--rbf", str(directory / "top.rbf"),
         "--write", str(directory / "routed.json"), "--report", str(directory / "timing.json")],
        directory / "route.log")
    assert (directory / "top.rbf").stat().st_size > 0
    report = json.loads((directory / "timing.json").read_text())
    assert report["utilization"][SDP]["used"] == 1
    assert report["utilization"][HPS]["used"] == 1
    assert report["utilization"]["altera_pll"]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                   for clock in report["fmax"].values())

    routed = json.loads((directory / "routed.json").read_text())["modules"]["top"]["cells"]
    packed = routed[name]
    assert packed["type"] == SDP
    assert int(packed["parameters"].get("CFG_OUT_REG_A", 0)) == int(expected["a"])
    assert int(packed["parameters"].get("CFG_OUT_REG_B", 0)) == int(expected["b"])

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(directory / "top.rbf"),
         str(directory / "top.bt")], directory / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             (directory / "top.bt").read_text(), re.MULTILINE))
    for field, value in expected["fields"].items():
        assert fields.get(field, "async") == value, (directory, field, fields)


def reject(args, directory, design, message):
    directory.mkdir(parents=True, exist_ok=True)
    fixture = directory / "synth.json"
    fixture.write_text(json.dumps(design))
    result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture)], capture_output=True, text=True, timeout=120)
    (directory / "route.log").write_text(result.stdout + result.stderr)
    assert result.returncode != 0 and message in result.stdout + result.stderr, (message, result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    dual_clock = Path(__file__).with_name("dual_clock.v").resolve()
    base20, name20, cell20 = synthesize(args, output / "sdp20", dual_clock, SDP, {"WIDTH": 20})
    assert "B1DATA" in cell20["port_directions"]

    # A normal 20-bit SDP has only the B read output.  A registered B output
    # selects the bottom output register and keeps the top half asynchronous.
    registered20 = copy.deepcopy(base20)
    set_output_registers(registered20, name20, b=True)
    route(args, output / "sdp20" / "registered-b", registered20, name20,
          {"a": 0, "b": 1, "fields": {"B_OUTPUT_SEL": "REG"}})
    print("PASS: 20-bit SDP B output register", flush=True)

    # A 40-bit SDP B result spans both physical halves, so both output muxes
    # must select their registers even though the logical cell has one B1DATA.
    base40, name40, _ = synthesize(args, output / "sdp40", dual_clock, SDP, {"WIDTH": 40})
    registered40 = copy.deepcopy(base40)
    set_output_registers(registered40, name40, b=True)
    route(args, output / "sdp40" / "registered-b", registered40, name40,
          {"a": 0, "b": 1, "fields": {"A_OUTPUT_SEL": "REG", "B_OUTPUT_SEL": "REG"}})
    print("PASS: 40-bit SDP B output register spans both halves", flush=True)
    registered40_a = copy.deepcopy(base40)
    set_output_registers(registered40_a, name40, a=True)
    route(args, output / "sdp40" / "registered-a", registered40_a, name40,
          {"a": 1, "b": 0, "fields": {"A_OUTPUT_SEL": "REG", "B_OUTPUT_SEL": "REG"}})
    print("PASS: 40-bit SDP A request registers the whole word", flush=True)

    # True dual-port outputs are independently selectable.  Exercise both
    # sides together so each physical output clock path is covered.
    tdp_source = Path(__file__).with_name("true_dual_port.v").resolve()
    base_tdp, name_tdp, _ = synthesize(
        args, output / "tdp", tdp_source, TDP, {"WIDTH": 20, "SAME_CLOCK": 0})
    registered_tdp = copy.deepcopy(base_tdp)
    set_output_registers(registered_tdp, name_tdp, a=True, b=True)
    route(args, output / "tdp" / "registered-both", registered_tdp, name_tdp,
          {"a": 1, "b": 1, "fields": {"A_OUTPUT_SEL": "REG", "B_OUTPUT_SEL": "REG"}})
    print("PASS: true dual-port A/B output registers", flush=True)

    # Keep malformed combinations explicit: an A output does not exist in a
    # narrow SDP, and an asynchronous read cannot also be registered.
    invalid_sdp = copy.deepcopy(base20)
    set_output_registers(invalid_sdp, name20, a=True)
    reject(args, output / "invalid-sdp-a", invalid_sdp,
           "CFG_OUT_REG_A requires true dual-port or a 40-bit read")
    invalid_async = copy.deepcopy(base20)
    async_cell = invalid_async["modules"]["top"]["cells"][name20]
    async_cell["parameters"]["CFG_ASYNC_READ"] = 1
    async_cell["connections"].pop("B1EN", None)
    async_cell["port_directions"].pop("B1EN", None)
    set_output_registers(invalid_async, name20, b=True)
    reject(args, output / "invalid-async", invalid_async,
           "CFG_ASYNC_READ cannot use CFG_OUT_REG_A/B")
    print("PASS: invalid output-register combinations rejected", flush=True)


if __name__ == "__main__":
    main()
