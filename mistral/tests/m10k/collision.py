#!/usr/bin/env python3
"""Check explicit Cyclone V M10K read-during-write contracts.

The Cyclone V true-dual-port primitive has write-through (NEW_DATA) as its
only physical same-port result.  Cross-port collisions are unspecified, so a
caller may choose the weaker DONT_CARE contract.  OLD_DATA and
NEW_DATA_WITH_NBE_READ must be rejected rather than silently becoming the
device default.
"""

import argparse
import copy
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
CELL = "MISTRAL_M10K"
HPS = "cyclonev_hps_interface_mpu_general_purpose"
NEW = "NEW_DATA_NO_NBE_READ"
DONT_CARE = "DONT_CARE"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def synthesize(args, output):
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("true_dual_port.v").resolve()
    script = output / "synth.ys"
    script.write_text(
        f"read_verilog {source}\n"
        "chparam -set WIDTH 20 -set SAME_CLOCK 1 top\n"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        "select -assert-count 1 t:MISTRAL_M10K_TDP\n"
        f"write_json {output / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(script)], output / "synth.log")
    design = json.loads((output / "base.json").read_text())
    cells = design["modules"]["top"]["cells"]
    name, cell = next((n, c) for n, c in cells.items()
                       if c["type"] == "MISTRAL_M10K_TDP")
    assert cell["parameters"]["CFG_DBITS"] == format(20, "032b")
    return design, name


def route(args, output, design, name, expected):
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / "synth.json"
    fixture.write_text(json.dumps(design))
    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--compress-rbf", "--rbf", str(output / "top.rbf"),
         "--write", str(output / "routed.json"), "--report", str(output / "timing.json")],
        output / "route.log")
    assert (output / "top.rbf").stat().st_size > 0
    report = json.loads((output / "timing.json").read_text())
    assert report["utilization"][CELL]["used"] == 1
    assert report["utilization"][HPS]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                   for clock in report["fmax"].values())
    packed = json.loads((output / "routed.json").read_text())["modules"]["top"]["cells"][name]
    assert packed["type"] == CELL
    for key, value in expected.items():
        assert packed["parameters"].get(key) == value, (key, packed["parameters"])

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE, str(output / "top.rbf"),
         str(output / "top.bt")], output / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    fields = dict(re.findall(r"^s " + re.escape(site) + r":(\S+) (\S+)$",
                             (output / "top.bt").read_text(), re.MULTILINE))
    assert fields.get("TRUE_DUAL_PORT") == "1"
    assert fields.get("A_DATA_FLOW_THRU") == "1"
    assert fields.get("B_DATA_FLOW_THRU") == "1"


def reject(args, output, design, name, key, value, message):
    output.mkdir(parents=True, exist_ok=True)
    bad = copy.deepcopy(design)
    bad["modules"]["top"]["cells"][name]["parameters"][key] = value
    fixture = output / "synth.json"
    fixture.write_text(json.dumps(bad))
    result = subprocess.run(
        [str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture)], capture_output=True, text=True, timeout=120)
    (output / "route.log").write_text(result.stdout + result.stderr)
    assert result.returncode != 0 and message in result.stdout + result.stderr, \
        (key, value, result.stdout[-2000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    design, name = synthesize(args, output / "synth")

    defaults = copy.deepcopy(design)
    route(args, output / "default", defaults, name,
          {"CFG_RDW_MODE_A": NEW, "CFG_RDW_MODE_B": NEW,
           "CFG_RDW_MODE_MIXED": DONT_CARE})
    print("PASS: default TDP collision contract is explicit", flush=True)

    dont_care = copy.deepcopy(design)
    cell = dont_care["modules"]["top"]["cells"][name]
    cell["parameters"].update({"CFG_RDW_MODE_A": DONT_CARE,
                                "CFG_RDW_MODE_B": DONT_CARE,
                                "CFG_RDW_MODE_MIXED": DONT_CARE})
    route(args, output / "dont-care", dont_care, name,
          {"CFG_RDW_MODE_A": DONT_CARE, "CFG_RDW_MODE_B": DONT_CARE,
           "CFG_RDW_MODE_MIXED": DONT_CARE})
    print("PASS: cross-port DONT_CARE contract preserves write-through hardware", flush=True)

    numeric = copy.deepcopy(design)
    numeric["modules"]["top"]["cells"][name]["parameters"].update({
        "CFG_RDW_MODE_A": 1, "CFG_RDW_MODE_B": 0, "CFG_RDW_MODE_MIXED": 1})
    route(args, output / "numeric", numeric, name,
          {"CFG_RDW_MODE_A": DONT_CARE, "CFG_RDW_MODE_B": NEW,
           "CFG_RDW_MODE_MIXED": DONT_CARE})
    print("PASS: numeric collision mode codes normalize to canonical contracts", flush=True)

    reject(args, output / "invalid-old", design, name, "CFG_RDW_MODE_A",
           "OLD_DATA", "unsupported read-during-write mode")
    reject(args, output / "invalid-nbe", design, name, "CFG_RDW_MODE_B",
           "NEW_DATA_WITH_NBE_READ", "unsupported read-during-write mode")
    reject(args, output / "invalid-cross-port-new", design, name, "CFG_RDW_MODE_MIXED",
           "NEW_DATA_NO_NBE_READ", "unsupported read-during-write mode")
    invalid_sdp = copy.deepcopy(design)
    invalid_sdp["modules"]["top"]["cells"][name]["type"] = "MISTRAL_M10K"
    reject(args, output / "invalid-sdp-contract", invalid_sdp, name, "CFG_RDW_MODE_A",
           NEW, "read-during-write mode parameters require true dual-port mode")
    print("PASS: unsupported collision modes rejected", flush=True)


if __name__ == "__main__":
    main()
