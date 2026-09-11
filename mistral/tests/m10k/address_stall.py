#!/usr/bin/env python3
"""Route the M10K address-stall inputs from fabric nets.

The locked Yosys Mistral memory models predate these two primitive ports, so
the fixture adds the ports at the JSON boundary.  The physical Mistral BEL
already exposes ADDRSTALLA/B; this regression checks that the packer retains
the logical connections and that both pins receive real routes.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
M10K = "MISTRAL_M10K"
HPS = "cyclonev_hps_interface_mpu_general_purpose"


def run(command, log, timeout=120):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def route_and_check(args, output, label, design, name, pins):
    case = output / label
    case.mkdir(exist_ok=True)
    fixture = case / "synth.json"
    fixture.write_text(json.dumps(design))
    run([str(args.nextpnr.resolve()), "--device", DEVICE, "--freq", "50",
         "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()),
         "--json", str(fixture), "--write", str(case / "routed.json"),
         "--report", str(case / "timing.json"), "--compress-rbf",
         "--rbf", str(case / "top.rbf")], case / "route.log")

    report = json.loads((case / "timing.json").read_text())
    assert report["utilization"][M10K]["used"] == 1
    assert report["utilization"][HPS]["used"] == 1
    assert report["fmax"] and all(clock["achieved"] >= clock["constraint"] == 50
                                   for clock in report["fmax"].values())
    assert (case / "top.rbf").stat().st_size > 0

    routed = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"]
    packed = routed[name]
    assert packed["type"] == M10K
    for port in pins:
        assert packed["connections"].get(port), (port, packed)

    run([str(args.mistral_cv.resolve()), "decomp", DEVICE,
         str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
    _, x, y, _ = packed["attributes"]["NEXTPNR_BEL"].split(".")
    site = f"M10K.{int(x):03d}.{int(y):03d}"
    bitstream = (case / "top.bt").read_text()
    for pin in pins:
        assert re.search(r"^r \S+ " + re.escape(site + ":" + pin) + r"$",
                         bitstream, re.MULTILINE), pin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    # The true-dual-port fixture gives us two independently clocked
    # M10K ports and an HPS GP fabric source for the stall controls without
    # requiring a new Yosys pin.
    source = Path(__file__).with_name("true_dual_port.v").resolve()
    synth = output / "synth.ys"
    synth.write_text(
        f"read_verilog {source}\n"
        "chparam -set WIDTH 10 -set SAME_CLOCK 0 top\n"
        "synth_intel_alm -nolutram -nodsp -top top\n"
        "select -assert-count 1 t:MISTRAL_M10K_TDP\n"
        f"write_json {output / 'base.json'}\n"
    )
    run([str(args.yosys.resolve()), "-Q", "-T", "-s", str(synth)], output / "synth.log")
    design = json.loads((output / "base.json").read_text())
    cells = design["modules"]["top"]["cells"]
    name, cell = next((n, c) for n, c in cells.items()
                      if c["type"] == "MISTRAL_M10K_TDP")
    hps = next(c for c in cells.values() if c["type"] == HPS)

    # Use two independent HPS GP nets.  These are deliberately fabric routes,
    # rather than constants, so the test exercises the Mistral GOUT resources.
    for port, bit in (("ADDRSTALLA", 30), ("ADDRSTALLB", 31)):
        cell["connections"][port] = [hps["connections"]["gp_out"][bit]]
        cell["port_directions"][port] = "input"

    route_and_check(args, output, "fabric", design, name,
                    ("ADDRSTALLA", "ADDRSTALLB"))

    # Hard constants are folded to the packer's soft constant nets, but must
    # still be routed to the physical controls rather than silently dropped.
    constant = json.loads((output / "base.json").read_text())
    constant_cell = constant["modules"]["top"]["cells"][name]
    for port, value in (("ADDRSTALLA", ["0"]), ("ADDRSTALLB", ["1"])):
        constant_cell["connections"][port] = value
        constant_cell["port_directions"][port] = "input"
    route_and_check(args, output, "constants", constant, name,
                    ("ADDRSTALLA", "ADDRSTALLB"))
    print("PASS: TDP M10K fabric and constant ADDRSTALLA/B routes, RBF and 50 MHz timing")


if __name__ == "__main__":
    main()
