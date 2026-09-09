#!/usr/bin/env python3
"""Check dedicated single-bit SDR input-register packing."""
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
inputs = [cell for cell in routed.values() if cell["type"] == "MISTRAL_SDRIN"]
assert len(inputs) == 1, [(name, cell["type"]) for name, cell in routed.items()]
assert not any(cell["type"] == "MISTRAL_FF" for cell in routed.values())
assert inputs[0]["connections"].get("CLK") and inputs[0]["connections"].get("Q")
report = json.loads((o / "timing.json").read_text())
assert report["utilization"]["MISTRAL_IO"]["used"] == 3
assert report["utilization"]["MISTRAL_FF"]["used"] == 0
assert report["fmax"] == {}
run([a.mistral_cv, "decomp", "5CSEBA6U23I7", o / "top.rbf", o / "top.bt"], o / "decode.log")
bt = (o / "top.bt").read_text()
oracle = json.loads((f / "oracle/mapping.json").read_text())
assert oracle["device"] == "5CSEBA6U23I7"
for route in oracle["routes"]:
    assert route in bt, route
dqs_settings = dict(re.findall(r"^s DQS16\.060\.000:(\S+\.0) (\S+)", bt, re.MULTILINE))
assert dqs_settings == oracle["settings"], dqs_settings
print("PASS: one SDR input register, GPIO DATAIN.3/CLKIN.0 and DQS FIFO clock settings")


def variant(name, expected, edit=None, qsf=None, expect_failure=True):
    design = json.loads((o / "input.json").read_text())
    ff = next(cell for cell in design["modules"]["top"]["cells"].values() if cell["type"] == "MISTRAL_FF")
    if edit:
        edit(design, ff)
    input_path = o / (name + ".json")
    input_path.write_text(json.dumps(design))
    qsf_path = o / (name + ".qsf")
    qsf_path.write_text((f / "pins.qsf").read_text() if qsf is None else qsf)
    run(
        [
            a.nextpnr,
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            qsf_path,
            "--json",
            input_path,
            "--write",
            o / (name + ".routed.json"),
        ],
        o / (name + ".log"),
        success=not expect_failure,
    )
    if expect_failure:
        assert expected in (o / (name + ".log")).read_text()
    else:
        cells = json.loads((o / (name + ".routed.json")).read_text())["modules"]["top"]["cells"]
        assert not any(cell["type"] == "MISTRAL_SDRIN" for cell in cells.values())
        assert any(cell["type"] == "MISTRAL_FF" for cell in cells.values())


variant("enable", "constant ENA/ACLR", lambda _design, ff: ff["connections"].update(ENA=[6]))
variant("reset", "constant ENA/ACLR", lambda _design, ff: ff["connections"].update(ACLR=["0"]))
variant("load", "constant ENA/ACLR", lambda _design, ff: ff["connections"].update(SLOAD=[6]))
variant("clock", "register clock must be driven", lambda _design, ff: ff["connections"].update(CLK=["0"]))
variant("parameter", "unsupported register parameters", lambda _design, ff: ff["parameters"].update(UNSUPPORTED="1"))


def inverted_clock(design, ff):
    ff["connections"]["CLK"] = [9]
    design["modules"]["top"]["cells"]["inverted_clock"] = {
        "hide_name": 0,
        "type": "MISTRAL_NOT",
        "parameters": {},
        "attributes": {},
        "port_directions": {"A": "input", "Q": "output"},
        "connections": {"A": [6], "Q": [9]},
    }


variant("inverted-clock", "noninverted clock source", inverted_clock)


def data_fanout(design, _ff):
    design["modules"]["top"]["cells"]["data_fanout"] = {
        "hide_name": 0,
        "type": "MISTRAL_BUF",
        "parameters": {},
        "attributes": {},
        "port_directions": {"A": "input", "Q": "output"},
        "connections": {"A": [5], "Q": [9]},
    }


variant("data-fanout", "exactly one register data input", data_fanout)
variant(
    "disabled",
    "",
    qsf=(f / "pins.qsf").read_text().replace("REGISTER ON", "REGISTER OFF"),
    expect_failure=False,
)

for command in ("set_input_delay", "set_output_delay"):
    sdc = o / (command + ".sdc")
    sdc.write_text((f / "clocks.sdc").read_text() + command + " -max 2.0 [get_ports DATA]\n")
    run(
        [a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", f / "pins.qsf", "--sdc", sdc, "--json", o / "input.json"],
        o / (command + ".log"),
        success=False,
    )
    assert "Unsupported SDC command '" + command + "'" in (o / (command + ".log")).read_text()

print("PASS: SDR input opt-out and seven unsupported input/clock/control requests")
