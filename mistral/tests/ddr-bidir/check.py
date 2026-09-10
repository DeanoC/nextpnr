#!/usr/bin/env python3
"""Check one-bit Cyclone V DDR bidirectional I/O register packing."""
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
assert sum(cell["type"] == "altddio_bidir" for cell in cells.values()) == 1

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
bidir = [cell for cell in routed.values() if cell["type"] == "MISTRAL_DDRBIDIR"]
assert len(bidir) == 1
assert not any(cell["type"] == "altddio_bidir" for cell in routed.values())
for port in ("PAD", "D_H", "D_L", "CLK", "CLKIN", "OE", "O", "Q_H", "Q_L"):
    assert bidir[0]["connections"].get(port), port
assert not bidir[0]["connections"].get("OE_OUT")

report = json.loads((out / "timing.json").read_text())
assert report["utilization"]["MISTRAL_IO"]["used"] == 2
assert report["utilization"]["MISTRAL_FF"]["used"] == 0
assert report["utilization"]["altera_pll"]["used"] == 0
assert report["fmax"] == {}
assert "constraining clock net 'FPGA_CLK1_50' to 50.00 MHz" in route_log
assert "DDR bidirectional I/O: GPIO register setup/hold" in route_log

run([a.mistral_cv, "decomp", "5CSEBA6U23I7", out / "top.rbf", out / "top.bt"], out / "decomp.log")
bt = (out / "top.bt").read_text()
settings = dict(re.findall(r"^s DQS16\.089\.008:(\S+\.9) (\S+)", bt, re.MULTILINE))
assert settings == {
    "OUTREG_MODE_SEL.9": "DDR",
    "OUTREG_OUTPUT_SEL.9": "SEL_SDR_DELAY",
    "RBOE_LVL_FR_CLK_EN.9": "1",
    "RB_FIFO_WCLK_EN.9": "1",
    "RB_FIFO_WCLK_INV.9": "1",
}, settings
assert not re.search(r"^i GPIO\.089\.008\.1:DATAOUT\.[01] ", bt, re.MULTILINE)
routes = run([a.mistral_cv, "routes", "5CSEBA6U23I7", out / "top.rbf"], out / "routes.txt")
for route_name in (
    "GPIO.089.008.1:DATAOUT.0",
    "GPIO.089.008.1:DATAOUT.1",
    "GPIO.089.008.1:CLKOUT.0",
    "GPIO.089.008.1:CLKIN.0",
    "GPIO.089.008.1:OEIN.0",
    "GPIO.089.008.1:DATAIN.0",
    "GPIO.089.008.1:DATAIN.2",
    "GPIO.089.008.1:DATAIN.3",
):
    assert route_name in routes, route_name
print("PASS: fabric DDR bidirectional I/O uses both DQS register directions at 50 MHz", flush=True)


constant_oe = copy.deepcopy(design)
constant_oe["modules"]["top"]["cells"]["ddr"]["connections"]["oe"] = ["0"]
constant_oe_path = out / "constant-oe.json"
constant_oe_path.write_text(json.dumps(constant_oe))
constant_oe_log = run(
    base + ["--json", constant_oe_path, "--write", out / "constant-oe-routed.json"],
    out / "constant-oe.log",
)
constant_oe_cells = json.loads((out / "constant-oe-routed.json").read_text())["modules"]["top"]["cells"]
assert sum(cell["type"] == "MISTRAL_DDRBIDIR" for cell in constant_oe_cells.values()) == 1
assert "Program finished normally" in constant_oe_log


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
    "requires width=1",
)
reject_variant(
    "parameter",
    lambda cell: cell["parameters"].update(invert_output="ON"),
    "unsupported parameter",
)
reject_variant(
    "oe-register",
    lambda cell: cell["parameters"].update(oe_reg="REGISTERED"),
    "unsupported parameter",
)
reject_variant(
    "enable",
    lambda cell: cell["connections"].update(inclocken=[12]),
    "enable must be high",
)
reject_variant(
    "reset",
    lambda cell: cell["connections"].update(aclr=[12]),
    "set/clear controls low",
)
reject_variant(
    "clock",
    lambda cell: cell["connections"].update(inclock=[]),
    "inclock and outclock must be driven by the same clock",
)
reject_variant(
    "clock-constant",
    lambda cell: cell["connections"].update(outclock=["0"]),
    "inclock and outclock must be driven by the same clock",
)
reject_variant(
    "dynamic-clock",
    lambda cell: cell["connections"].update(outclock=[7]),
    "inclock and outclock must be driven by the same clock",
)
reject_variant(
    "output-alias",
    lambda cell: cell["connections"].update(dataout_l=cell["connections"]["dataout_h"]),
    "multiply driven",
)
reject_variant(
    "oe-out",
    lambda cell: cell["connections"].update(oe_out=[6]),
    "multiply driven",
)
print("PASS: ten invalid DDR bidirectional I/O requests rejected", flush=True)
