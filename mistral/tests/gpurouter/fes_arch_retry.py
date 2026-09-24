#!/usr/bin/env python3
"""Check that a Coleco scaffold route finishes or reports a bounded bind failure.

The fixture directory is the build/coleco-bus-diagnostic/<recipe> directory
created by the FES producer. It contains the authenticated scaffold and cart
inputs, so this test exercises the real final Arch binding path.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--require-route", action="store_true")
    args = parser.parse_args()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # A restored scaffold carries its router settings in JSON; those replace
    # the command-line settings when --json loads it. Bound this replay through
    # the copied fixture so it does not need an external process killer.
    scaffold = json.loads((fixture / "scaffold.json").read_text())
    # "10" is read as a binary property by the JSON loader; "12" is the
    # decimal setting needed to allow the six normal convergence iterations.
    scaffold["modules"]["top"]["settings"]["gpurouter/maxIter"] = "12"
    scaffold_path = output / "scaffold.json"
    scaffold_path.write_text(json.dumps(scaffold, separators=(",", ":")) + "\n")
    command = [
        str(args.nextpnr.resolve()), "--verbose", "--device", "5CSEBA6U23I7",
        "--json", str(scaffold_path),
        "--qsf", str(fixture / "cart.qsf"),
        "--sdc", str(fixture / "clocks.sdc"), "--freq", "52.224",
        "--fes-scaffold", "--fes-cart", str(fixture / "cart.json"),
        "--fes-slot-clock", "system_clock.clocks[0]",
        "--fes-cram-region", "1769,32,2806,1034",
        "--no-pack", "--seed", "4", "--router", "gpu",
        "--rbf", str(output / "cart.rbf"),
        "--compress-rbf", "--write", str(output / "routed.json"),
        "--report", str(output / "timing.json"),
    ]
    env = dict(os.environ, HIP_VISIBLE_DEVICES=str(args.gpu))
    log_path = output / "route.log"
    try:
        with log_path.open("w") as log:
            result = subprocess.run(command, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=args.timeout)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit("FAIL: architecture bind retries did not terminate") from exc
    log = log_path.read_text(errors="replace")
    if result.returncode == 0:
        if "Routing complete." not in log or not (output / "cart.rbf").is_file():
            raise SystemExit("FAIL: successful route did not produce a cart RBF")
        print("PASS: Coleco GPU scaffold route completed")
    elif (not args.require_route and "architecture bind" in log
          and "plug_request[23]" in log and "ERROR:" in log):
        print("PASS: Coleco architecture rejection was reported and bounded")
    else:
        raise SystemExit(f"FAIL: route exited {result.returncode} without a bounded bind report; see {log_path}")


if __name__ == "__main__":
    main()
