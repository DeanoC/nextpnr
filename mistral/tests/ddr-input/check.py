#!/usr/bin/env python3
"""Check one-bit Cyclone V DDR input-register packing."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for key in ("yosys", "nextpnr", "mistral-cv", "output"):
    parser.add_argument("--" + key, required=True, type=Path)
a = parser.parse_args()
f = Path(__file__).resolve().parent
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)


def run(command, log, success=True):
    result = subprocess.run(
        [str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180
    )
    log.write_text(result.stdout)
    assert (result.returncode == 0) == success, result.stdout[-3000:]
    return result.stdout


run(
    [
        a.yosys,
        "-p",
        f"read_verilog {f / 'top.v'}; synth_intel_alm -nobram -nolutram -nodsp -top top; write_json {o / 'input.json'}",
    ],
    o / "synth.log",
)
design = json.loads((o / "input.json").read_text())
cells = design["modules"]["top"]["cells"]
assert sum(cell["type"] == "altddio_in" for cell in cells.values()) == 1
run(
    [
        a.nextpnr,
        "--device",
        "5CSEBA6U23I7",
        "--qsf",
        f / "pins.qsf",
        "--sdc",
        f / "clocks.sdc",
        "--json",
        o / "input.json",
        "--write",
        o / "routed.json",
        "--report",
        o / "timing.json",
        "--rbf",
        o / "top.rbf",
        "--compress-rbf",
    ],
    o / "route.log",
)
routed = json.loads((o / "routed.json").read_text())["modules"]["top"]["cells"]
ddr = [cell for cell in routed.values() if cell["type"] == "MISTRAL_DDRIN"]
assert len(ddr) == 1
assert not any(cell["type"] == "altddio_in" for cell in routed.values())
assert ddr[0]["connections"].get("CLK")
assert ddr[0]["connections"].get("Q_H")
assert ddr[0]["connections"].get("Q_L")
report = json.loads((o / "timing.json").read_text())
assert report["utilization"]["MISTRAL_IO"]["used"] == 4
assert report["utilization"]["MISTRAL_FF"]["used"] == 0
assert report["fmax"] == {}
run([a.mistral_cv, "decomp", "5CSEBA6U23I7", o / "top.rbf", o / "top.bt"], o / "decode.log")
bt = (o / "top.bt").read_text()
oracle = json.loads((f / "oracle/mapping.json").read_text())
for route in oracle["routes"]:
    assert route in bt, route
dqs_settings = dict(re.findall(r"^s DQS16\.060\.000:(\S+\.0) (\S+)", bt, re.MULTILINE))
assert dqs_settings == oracle["settings"], dqs_settings
print("PASS: one DDR input register, GPIO high/low DATAIN lanes and DQS settings")


def reject_variant(name, edit, expected):
    design = json.loads((o / "input.json").read_text())
    edit(design)
    path = o / (name + ".json")
    path.write_text(json.dumps(design))
    log = run(
        [
            a.nextpnr,
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            f / "pins.qsf",
            "--sdc",
            f / "clocks.sdc",
            "--json",
            path,
        ],
        o / (name + ".log"),
        success=False,
    )
    assert expected in log, (name, log[-2000:])


def ddr(design):
    return design["modules"]["top"]["cells"]["ddr"]


reject_variant(
    "width",
    lambda design: ddr(design)["parameters"].update(width="00000000000000000000000000000010"),
    "input capture requires width=1",
)
reject_variant(
    "parameter",
    lambda design: ddr(design)["parameters"].update(invert_input_clocks="ON"),
    "unsupported parameter",
)
reject_variant(
    "enable",
    lambda design: ddr(design)["connections"].update(inclocken=[6]),
    "enable must be high",
)
reject_variant(
    "reset",
    lambda design: ddr(design)["connections"].update(aclr=[6]),
    "enable must be high",
)
reject_variant(
    "clock-constant",
    lambda design: ddr(design)["connections"].update(inclock=["0"]),
    "inclock must be driven by a clock",
)
reject_variant(
    "equal-outputs",
    lambda design: ddr(design)["connections"].update(dataout_l=[8]),
    "multiply driven",
)


def inverted_clock(design):
    ddr(design)["connections"]["inclock"] = [10]
    design["modules"]["top"]["cells"]["inverted_clock"] = {
        "hide_name": 0,
        "type": "MISTRAL_NOT",
        "parameters": {},
        "attributes": {},
        "port_directions": {"A": "input", "Q": "output"},
        "connections": {"A": [7], "Q": [10]},
    }


reject_variant("inverted-clock", inverted_clock, "noninverted clock source")


def data_fanout(design):
    ddr(design)["connections"]["datain"] = [10]
    design["modules"]["top"]["cells"]["data_fanout"] = {
        "hide_name": 0,
        "type": "MISTRAL_BUF",
        "parameters": {},
        "attributes": {},
        "port_directions": {"A": "input", "Q": "output"},
        "connections": {"A": [6], "Q": [10]},
    }


reject_variant("data-buffer", data_fanout, "driven directly by one input buffer")
print("PASS: eight invalid DDR input requests rejected")
