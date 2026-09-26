#!/usr/bin/env python3
"""Check M10K ACLR defaults, constants, fabric routes and output registers."""

import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
CELL_SDP = "MISTRAL_M10K"
CELL_TDP = "MISTRAL_M10K_TDP"
HPS = "cyclonev_hps_interface_mpu_general_purpose"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def synthesize(args, source, case, cell_type, extra):
    script = case / "synth.ys"
    script.write_text(
        f"read_verilog {source}\n"
        f"{extra}"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        f"select -assert-count 1 t:{cell_type}\n"
        f"write_json {case / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)], case / "synth.log")
    design = json.loads((case / "base.json").read_text())
    cells = design["modules"]["top"]["cells"]
    name, _ = next((name, cell) for name, cell in cells.items()
                   if cell["type"] == cell_type)
    return design, name


def set_aclr(design, name, values):
    cell = design["modules"]["top"]["cells"][name]
    for port, bits in values.items():
        cell["connections"][port] = bits
        cell["port_directions"][port] = "input"


def route(args, case, design, name, expected):
    fixture = case / "synth.json"
    fixture.write_text(json.dumps(design))
    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--compress-rbf", "--rbf", str(case / "top.rbf"),
         "--write", str(case / "routed.json"), "--report", str(case / "timing.json")],
        case / "route.log")

    rbf = case / "top.rbf"
    assert rbf.stat().st_size > 0
    report = json.loads((case / "timing.json").read_text())
    assert report["utilization"][CELL_SDP]["used"] == 1
    assert report["utilization"][HPS]["used"] == 1
    assert report["utilization"]["altera_pll"]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                   for clock in report["fmax"].values())

    routed = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"]
    packed = routed[name]
    assert packed["type"] == CELL_SDP
    for port, connected in expected.get("connections", {}).items():
        assert bool(packed["connections"].get(port)) == connected, (port, packed)

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(rbf),
         str(case / "top.bt")], case / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    bitstream = (case / "top.bt").read_text()
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             bitstream, re.MULTILINE))
    for field, value in expected.get("fields", {}).items():
        assert fields.get(field, "0") == value, (case, field, fields)
    for field in expected.get("absent_fields", ()):
        assert field not in fields, (case, field, fields)
    for pin, present in expected.get("routes", {}).items():
        found = re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$",
                          bitstream, re.MULTILINE)
        assert bool(found) == present, (case, pin, bitstream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    sdp_case = output / "sdp"
    sdp_case.mkdir(exist_ok=True)
    sdp_source = Path(__file__).with_name("dual_clock.v").resolve()
    base, name = synthesize(args, sdp_source, sdp_case, CELL_SDP,
                            "chparam -set WIDTH 20 top\n")
    hps = next(c for c in base["modules"]["top"]["cells"].values() if c["type"] == HPS)
    fabric0, fabric1 = hps["connections"]["gp_out"][5], hps["connections"]["gp_out"][30]

    cases = (
        ("omitted", {}, {
            "connections": {"ACLR0": False, "ACLR1": False},
            "absent_fields": ("A_OUTCLR_EN", "B_OUTCLR_EN", "TOP_OUTCLR_SEL",
                               "BOT_1_OUTCLR_SEL", "TOP_CLR_INV", "BOT_CLR_INV"),
            "routes": {"ACLR.0": False, "ACLR.1": False},
        }),
        ("constants", {"ACLR0": ["0"], "ACLR1": ["1"]}, {
            "connections": {"ACLR0": False, "ACLR1": False},
            "fields": {"B_OUTCLR_EN": "REG",
                        "B_OUTPUT_SEL": "REG", "BOT_1_OUTCLR_SEL": "1"},
            "absent_fields": ("A_OUTCLR_EN", "TOP_CLR_INV", "BOT_CLR_INV"),
            "routes": {"ACLR.0": False, "ACLR.1": False},
        }),
        ("fabric", {"ACLR0": [fabric0], "ACLR1": [fabric1]}, {
            "connections": {"ACLR0": True, "ACLR1": True},
            "fields": {"B_OUTCLR_EN": "REG", "B_OUTPUT_SEL": "REG",
                        "BOT_1_OUTCLR_SEL": "1"},
            "routes": {"ACLR.0": True, "ACLR.1": True},
        }),
    )
    for label, values, expected in cases:
        case = sdp_case / label
        case.mkdir(exist_ok=True)
        design = copy.deepcopy(base)
        set_aclr(design, name, values)
        route(args, case, design, name, expected)
        print(f"PASS: SDP {label}: ACLR defaults/constants/routes and 50 MHz timing", flush=True)

    sdp40_case = output / "sdp40"
    sdp40_case.mkdir(exist_ok=True)
    sdp40, sdp40_name = synthesize(args, sdp_source, sdp40_case, CELL_SDP,
                                   "chparam -set WIDTH 40 top\n")
    sdp40_hps = next(c for c in sdp40["modules"]["top"]["cells"].values() if c["type"] == HPS)
    sdp40_values = {"ACLR0": [sdp40_hps["connections"]["gp_out"][5]],
                    "ACLR1": [sdp40_hps["connections"]["gp_out"][30]]}
    sdp40_design = copy.deepcopy(sdp40)
    set_aclr(sdp40_design, sdp40_name, sdp40_values)
    sdp40_route_case = sdp40_case / "fabric-both"
    sdp40_route_case.mkdir(exist_ok=True)
    route(args, sdp40_route_case, sdp40_design, sdp40_name, {
        "connections": {"ACLR0": True, "ACLR1": True},
        "fields": {"A_OUTCLR_EN": "REG", "A_OUTPUT_SEL": "REG",
                    "B_OUTCLR_EN": "REG", "B_OUTPUT_SEL": "REG",
                    "TOP_OUTCLR_SEL": "1", "BOT_1_OUTCLR_SEL": "1"},
        "routes": {"ACLR.0": True, "ACLR.1": True},
    })
    print("PASS: SDP 40-bit fabric ACLR spans both output halves", flush=True)

    tdp_case = output / "tdp"
    tdp_case.mkdir(exist_ok=True)
    tdp_source = Path(__file__).with_name("true_dual_port.v").resolve()
    tdp, tdp_name = synthesize(
        args, tdp_source, tdp_case, CELL_TDP,
        "chparam -set WIDTH 20 -set SAME_CLOCK 0 top\n")
    tdp_hps = next(c for c in tdp["modules"]["top"]["cells"].values() if c["type"] == HPS)
    tdp0, tdp1 = tdp_hps["connections"]["gp_out"][5], tdp_hps["connections"]["gp_out"][30]
    inverted_net = next(cell["connections"]["Q"][0]
                        for cell in tdp["modules"]["top"]["cells"].values()
                        if cell["type"] == "MISTRAL_NOT")
    tdp_cases = (
        ("constant-a", {"ACLR0": ["1"], "ACLR1": ["0"]}, {
            "connections": {"ACLR0": False, "ACLR1": False},
            "fields": {"A_OUTCLR_EN": "REG", "A_OUTPUT_SEL": "REG"},
            "absent_fields": ("B_OUTCLR_EN", "TOP_OUTCLR_SEL", "BOT_1_OUTCLR_SEL",
                               "TOP_CLR_INV", "BOT_CLR_INV"),
            "routes": {"ACLR.0": False, "ACLR.1": False},
        }),
        ("fabric-both", {"ACLR0": [tdp0], "ACLR1": [tdp1]}, {
            "connections": {"ACLR0": True, "ACLR1": True},
            "fields": {"A_OUTCLR_EN": "REG", "A_OUTPUT_SEL": "REG",
                        "B_OUTCLR_EN": "REG", "B_OUTPUT_SEL": "REG",
                        "BOT_1_OUTCLR_SEL": "1"},
            "routes": {"ACLR.0": True, "ACLR.1": True},
        }),
        ("inverted-b", {"ACLR1": [inverted_net]}, {
            "connections": {"ACLR0": False, "ACLR1": True},
            "fields": {"B_OUTCLR_EN": "REG", "B_OUTPUT_SEL": "REG",
                        "BOT_CLR_INV": "1", "BOT_1_OUTCLR_SEL": "1"},
            "absent_fields": ("A_OUTCLR_EN",),
            "routes": {"ACLR.0": False, "ACLR.1": True},
        }),
    )
    for label, values, expected in tdp_cases:
        case = tdp_case / label
        case.mkdir(exist_ok=True)
        design = copy.deepcopy(tdp)
        set_aclr(design, tdp_name, values)
        route(args, case, design, tdp_name, expected)
        print(f"PASS: TDP {label}: ACLR lanes, output registers and 50 MHz timing", flush=True)


if __name__ == "__main__":
    main()
