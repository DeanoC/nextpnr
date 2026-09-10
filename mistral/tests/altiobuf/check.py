#!/usr/bin/env python3
"""Check direct Intel altiobuf primitives normalize onto Cyclone V GPIO BELs."""
import argparse
import copy
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for key in ("yosys", "nextpnr", "output"):
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
assert sum(cell["type"] == "altiobuf_in" for cell in cells.values()) == 1
assert sum(cell["type"] == "altiobuf_out" for cell in cells.values()) == 1
assert sum(cell["type"] == "altiobuf_bidir" for cell in cells.values()) == 1

route = [
    a.nextpnr,
    "--device",
    "5CSEBA6U23I7",
    "--qsf",
    fixture / "pins.qsf",
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
assert (out / "top.rbf").is_file() and (out / "top.rbf").stat().st_size > 0

routed = json.loads((out / "routed.json").read_text())["modules"]["top"]["cells"]
assert not any(cell["type"].startswith("altiobuf_") for cell in routed.values())
assert sum(cell["type"] == "MISTRAL_IB" for cell in routed.values()) == 2
assert sum(cell["type"] == "MISTRAL_OB" for cell in routed.values()) == 2
assert sum(cell["type"] == "MISTRAL_IO" for cell in routed.values()) == 1
report = json.loads((out / "timing.json").read_text())
assert report["utilization"]["MISTRAL_IO"]["used"] == 5
assert report["utilization"]["MISTRAL_FF"]["used"] == 0
assert report["utilization"]["altera_pll"]["used"] == 0
assert "Program finished normally" in route_log
io = next(cell for cell in routed.values() if cell["type"] == "MISTRAL_IO")
assert io["connections"].get("I")
assert io["connections"].get("OE")
assert io["connections"].get("O")
bidir_ob = next(
    cell
    for cell in routed.values()
    if cell["type"] == "MISTRAL_OB" and cell["connections"].get("I") == io["connections"]["O"]
)
assert bidir_ob["connections"]["I"] == io["connections"]["O"]
print("PASS: direct altiobuf primitives use existing GPIO BELs", flush=True)


def write_variant(name, variant):
    case = out / name
    case.mkdir(parents=True, exist_ok=True)
    path = case / "input.json"
    path.write_text(json.dumps(variant))
    return case, path


def route_variant(name, variant, success=True):
    case, path = write_variant(name, variant)
    command = [a.nextpnr, "--device", "5CSEBA6U23I7", "--qsf", fixture / "pins.qsf", "--json", path]
    if success:
        command += [
            "--write",
            case / "routed.json",
            "--report",
            case / "timing.json",
            "--rbf",
            case / "top.rbf",
            "--compress-rbf",
        ]
    log = run(command, case / "route.log", success=success)
    return case, log


def primitive_variant(primitive_type, edit):
    variant = copy.deepcopy(design)
    cell = next(cell for cell in variant["modules"]["top"]["cells"].values() if cell["type"] == primitive_type)
    edit(cell)
    return variant


for value in ("0", "1"):
    case, _ = route_variant(
        "bidir-oe-constant-" + value,
        primitive_variant("altiobuf_bidir", lambda cell, value=value: cell["connections"].update(oe=[value])),
    )
    packed = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"].values()
    io = next(cell for cell in packed if cell["type"] == "MISTRAL_IO")
    assert io["connections"].get("OE"), value
print("PASS: bidirectional OE constants remain connected", flush=True)


for name, primitive_type, edit, message in (
    (
        "input-width",
        "altiobuf_in",
        lambda cell: cell["parameters"].update(number_of_channels="00000000000000000000000000000010"),
        "altiobuf input",
    ),
    (
        "output-oe",
        "altiobuf_out",
        lambda cell: cell["parameters"].update(use_oe="TRUE"),
        "altiobuf output",
    ),
    (
        "bidir-bus-hold",
        "altiobuf_bidir",
        lambda cell: cell["parameters"].update(enable_bus_hold="ON"),
        "altiobuf bidirectional I/O",
    ),
    (
        "bidir-width",
        "altiobuf_bidir",
        lambda cell: cell["parameters"].update(number_of_channels="00000000000000000000000000000010"),
        "altiobuf bidirectional I/O",
    ),
):
    _, log = route_variant(name, primitive_variant(primitive_type, edit), success=False)
    assert message in log, (name, log[-3000:])
print("PASS: unsupported altiobuf profiles fail before placement", flush=True)
