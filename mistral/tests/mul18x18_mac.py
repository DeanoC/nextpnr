#!/usr/bin/env python3
"""Route the M18X18P36 multiply-add and cascade controls."""

import argparse
import json
from pathlib import Path
import re
import subprocess


def make_fixture(path: Path) -> dict:
    design = json.loads(path.read_text())
    module = design["modules"]["top"]
    cells = module["cells"]
    names = [name for name, cell in cells.items() if cell["type"] == "MISTRAL_MUL9X9"]
    assert len(names) == 1, names
    mul = cells[names[0]]
    mul["type"] = "MISTRAL_MUL18X18"
    mul["parameters"].update(CASCADE_EN="1", CASCADE_1ST_EN="1", CHAIN_OUTPUT_EN="1")
    mul["connections"]["A"] = list(mul["connections"]["A"]) + ["0"] * 9
    mul["connections"]["B"] = list(mul["connections"]["B"]) + ["0"] * 9
    mul["port_directions"]["C"] = "input"
    mul["connections"]["C"] = list(mul["connections"]["A"][:9]) * 4
    mul["port_directions"]["ACCUMULATE"] = "input"
    mul["connections"]["ACCUMULATE"] = [4]
    mul["port_directions"]["NEGATE"] = "input"
    mul["connections"]["NEGATE"] = [4]
    mul["port_directions"]["LOADCONST"] = "input"
    mul["connections"]["LOADCONST"] = [4]
    mul["port_directions"]["SUB"] = "input"
    mul["connections"]["SUB"] = [4]

    used = [
        bit
        for cell in cells.values()
        for bits in cell.get("connections", {}).values()
        for bit in bits
        if isinstance(bit, int)
    ]
    next_net = max(used) + 1
    mul["connections"]["Y"] = list(mul["connections"]["Y"]) + list(range(next_net, next_net + 18))
    return design


def run(command, log: Path):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "mistral-cv", "fixture", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = out / "synth.json"
    fixture.write_text(json.dumps(make_fixture(args.fixture)))
    run(
        [
            str(args.nextpnr.resolve()),
            "--json",
            str(fixture),
            "--device",
            "5CSEBA6U23I7",
            "--qsf",
            str(args.qsf.resolve()),
            "--sdc",
            str(args.sdc.resolve()),
            "--freq",
            "50",
            "--compress-rbf",
            "--rbf",
            str(out / "top.rbf"),
            "--write",
            str(out / "routed.json"),
            "--report",
            str(out / "timing.json"),
            "--detailed-timing-report",
        ],
        out / "route.log",
    )

    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["MISTRAL_MUL18X18"] == {"used": 1, "available": 112}, util
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    assert util["MISTRAL_MUL9X9"]["used"] == 0
    assert util["MISTRAL_M10K"]["used"] == 0
    assert util["altera_pll"]["used"] == 0
    assert report["fmax"]["product.FPGA_CLK1_50"]["achieved"] >= 50

    run(
        [
            str(args.mistral_cv.resolve()),
            "decomp",
            "5CSEBA6U23I7",
            str(out / "top.rbf"),
            str(out / "top.bt"),
        ],
        out / "decomp.log",
    )
    bt = (out / "top.bt").read_text()
    sites = re.findall(r"^s (DSP\.\d+\.\d+):MODE M18X18P36$", bt, re.M)
    assert len(sites) == 1, sites
    site = sites[0]
    settings = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$", bt, re.M))
    assert settings["CASCADE_EN"] == "1", settings
    assert settings["CASCADE_1ST_EN"] == "1", settings
    assert settings["CHAIN_OUTPUT_EN"] == "1", settings
    for group in (6, 7, 8, 9):
        assert f"{site}.{group}:DATAIN." in bt
    for control in ("ACCUMULATE", "SUB", "NEGATE", "LOADCONST"):
        assert f"{site}:{control}" in bt
    assert (out / "top.rbf").read_bytes()
    print("PASS", site)


if __name__ == "__main__":
    main()
