#!/usr/bin/env python3
"""Check one-bit Cyclone V DDR output registers with fabric data."""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
for key in ("yosys", "nextpnr", "mistral-cv", "output"):
    parser.add_argument("--" + key, required=True, type=Path)
a = parser.parse_args()
fixture = Path(__file__).resolve().parent
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)


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
        f"read_verilog {fixture / 'top.v'}; synth_intel_alm -nobram -nolutram -nodsp -top top; "
        f"write_json {out / 'input.json'}",
    ],
    out / "synth.log",
)
design = json.loads((out / "input.json").read_text())
cells = design["modules"]["top"]["cells"]
assert sum(cell["type"] == "altddio_out" for cell in cells.values()) == 1

base = [
    a.nextpnr,
    "--device",
    "5CSEBA6U23I7",
    "--qsf",
    fixture / "pins.qsf",
    "--sdc",
    fixture / "clocks.sdc",
    "--freq",
    "50",
]
route = base + [
    "--json",
    out / "input.json",
    "--write",
    out / "routed.json",
    "--report",
    out / "timing.json",
    "--rbf",
    out / "top.rbf",
    "--compress-rbf",
]
route_log = run(route, out / "route.log")

routed = json.loads((out / "routed.json").read_text())["modules"]["top"]["cells"]
ddr = [cell for cell in routed.values() if cell["type"] == "MISTRAL_DDROUT"]
assert len(ddr) == 1
assert not any(cell["type"] == "altddio_out" for cell in routed.values())
assert ddr[0]["connections"].get("CLK")
assert ddr[0]["connections"].get("D_H")
assert ddr[0]["connections"].get("D_L")
assert "DDR_HIGH" not in ddr[0].get("parameters", {})

report = json.loads((out / "timing.json").read_text())
assert report["utilization"]["MISTRAL_IO"]["used"] == 2
assert report["utilization"]["MISTRAL_FF"]["used"] == 2
assert report["utilization"]["MISTRAL_CLKENA"]["used"] == 1
assert report["utilization"]["MISTRAL_MUL9X9"]["used"] == 0
assert report["utilization"]["MISTRAL_MUL18X18"]["used"] == 0
assert report["utilization"]["MISTRAL_MUL27X27"]["used"] == 0
assert report["utilization"]["altera_pll"]["used"] == 0
assert report["fmax"]
assert all(clock["constraint"] == 50 and clock["achieved"] >= 50 for clock in report["fmax"].values())
assert "DDR output data: GPIO register setup/hold and clock-to-pad timing are uncharacterized" in route_log

run(
    [a.mistral_cv, "decomp", "5CSEBA6U23I7", out / "top.rbf", out / "top.bt"],
    out / "decomp.log",
)
bt = (out / "top.bt").read_text()
oracle = json.loads((fixture / "oracle/mapping.json").read_text())
for route_name in oracle["routes"]:
    assert f"{route_name} ; W15" in bt, route_name
assert not re.search(r"^i GPIO\.089\.008\.1:DATAOUT\.[01] ", bt, re.MULTILINE)
dqs_settings = dict(re.findall(r"^s DQS16\.089\.008:(\S+\.9) (\S+)", bt, re.MULTILINE))
assert dqs_settings == oracle["settings"], dqs_settings
routes = run(
    [a.mistral_cv, "routes", "5CSEBA6U23I7", out / "top.rbf"],
    out / "routes.txt",
)
for route_name in oracle["routes"]:
    assert route_name in routes, route_name
print("PASS: fabric DDR data packed onto both DATAOUT lanes at 50 MHz", flush=True)


def reject_variant(name, edit, expected):
    variant = copy.deepcopy(design)
    edit(variant["modules"]["top"]["cells"]["ddr"])
    path = out / (name + ".json")
    path.write_text(json.dumps(variant))
    log = run(base + ["--json", path], out / (name + ".log"), success=False)
    assert expected in log, (name, log[-2000:])


reject_variant(
    "width",
    lambda cell: cell["parameters"].update(width="00000000000000000000000000000010"),
    "DDR output requires width=1",
)
reject_variant(
    "mixed-constant",
    lambda cell: cell["connections"].update(datain_h=["1"]),
    "fabric DDR data requires two connected nonconstant datain_h/datain_l nets",
)
reject_variant(
    "equal-constant",
    lambda cell: cell["connections"].update(datain_h=["1"], datain_l=["1"]),
    "fabric DDR data requires two connected nonconstant datain_h/datain_l nets",
)
reject_variant(
    "missing-data",
    lambda cell: cell["connections"].update(datain_h=[]),
    "fabric DDR data requires two connected nonconstant datain_h/datain_l nets",
)
reject_variant(
    "clock",
    lambda cell: cell["connections"].update(outclock=[]),
    "outclock must be driven by a clock",
)
reject_variant(
    "clock-constant",
    lambda cell: cell["connections"].update(outclock=["0"]),
    "outclock must be driven by a clock",
)
reject_variant(
    "reset",
    lambda cell: cell["connections"].update(aclr=["1"]),
    "enable/OE must be constant high and resets constant low",
)
reject_variant(
    "oe",
    lambda cell: cell["connections"].update(oe=["0"]),
    "enable/OE must be constant high and resets constant low",
)
reject_variant(
    "enable",
    lambda cell: cell["connections"].update(outclocken=["0"]),
    "enable/OE must be constant high and resets constant low",
)
reject_variant(
    "invert",
    lambda cell: cell["parameters"].update(invert_output="ON"),
    "unsupported parameter",
)
print("PASS: ten invalid DDR output requests rejected", flush=True)
