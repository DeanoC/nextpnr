#!/usr/bin/env python3
"""Replay FES #237's retained ZX81 fixture through the complete cart route.

--fixture contains shell/{routed.json,socket.qsf} and cart/cart.json copied
from the failing FES producer outputs. The original fixture is never changed.
"""

import argparse
import json
from pathlib import Path
import subprocess


def routing(module, net):
    return set(module["netnames"][net]["attributes"]["ROUTING"].split(";"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "fixture", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    fixture, output = args.fixture.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    design = json.loads((fixture / "shell/routed.json").read_text())
    shell = design["modules"]["top"]
    # Loaded JSON settings override command-line settings. Force the reference
    # backend in our private copy as well, so this regression never claims a GPU.
    shell["settings"]["gpurouter/cpu"] = "1"
    scaffold = output / "shell.json"
    scaffold.write_text(json.dumps(design))
    protected = "WM.18.1.0.H14.19.1.0"
    removable = "H14.19.1.0.H3.27.1.4"
    old = routing(shell, "$PACKER_GND_NET")
    assert protected in old and removable in old, "wrong regression fixture"
    command = [
        str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
        "--json", str(scaffold),
        "--qsf", str(fixture / "shell/socket.qsf"),
        "--fes-scaffold", "--fes-cart", str(fixture / "cart/cart.json"),
        "--fes-slot-clock", "clk_sys", "--fes-cram-region", "1769,32,2806,7024",
        "--sdc", str(fixture / "cart/clocks.sdc"), "--freq", "52",
        "--no-pack", "--router", "gpu", "--gpu-cpu", "--seed", "2",
        "--write", str(output / "routed.json"), "--report", str(output / "timing.json"),
        "--rbf", str(output / "cart.rbf"), "--compress-rbf",
    ]
    with (output / "route.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    merged = json.loads((output / "routed.json").read_text())["modules"]["top"]
    remaining = routing(merged, "$PACKER_GND_NET")
    assert protected in remaining, "cart route removed the frozen mux controlling CRAM (1647,132)"
    assert removable not in remaining, "obsolete in-slot response branch must still be trimmed"
    assert "WM.18.1.0" in remaining, "protected route lost its source wire"
    print("PASS: outside CRAM mux survives ZX81 route; obsolete in-slot branch is removed")


if __name__ == "__main__":
    main()
