#!/usr/bin/env python3
"""Route two pad clocks and require each net to reach that pad's DCMUX.

``pad_clock.py`` builds route strings from the same table it checks, so a
wrong pad-to-DCMUX map can still pass. These fits are nextpnr routes on
5CSEBA6U23I7. The right-edge output register is ``GPIO.89.8.1:CLKOUT.0``
(DCMUX z 8, not the pad index). The top-edge input register is
``GPIO.64.0.0:CLKIN.0`` (DCMUX z 10). libmistral chooses those wires; this
test only accepts the route when the table names the same DCMUX.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pad_clock import clock_route_ok

_CASES = (
    {
        "name": "right-clkout",
        "fixture": "sdr-output",
        "route": "GPIO.89.8.1:CLKOUT.0",
        "packed": "MISTRAL_SDROUT",
        "freq": True,
    },
    {
        "name": "top-clkin",
        "fixture": "sdr-input",
        "route": "GPIO.64.0.0:CLKIN.0",
        "packed": "MISTRAL_SDRIN",
        "freq": False,
    },
)


def _run(command, log):
    result = subprocess.run(
        [str(item) for item in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=600,
    )
    log.write_text(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(f"{command[0]} exited {result.returncode}:\n{result.stdout[-4000:]}")
    return result.stdout


def _clock_routings(routed_path):
    design = json.loads(routed_path.read_text())["modules"]["top"]
    lines = []
    for name, net in design["netnames"].items():
        routing = net.get("attributes", {}).get("ROUTING", "")
        if "DCMUX" in routing:
            lines.append(f"{name}: {routing}")
    return lines


def _route_case(args, case):
    fixture = Path(__file__).resolve().parent / case["fixture"]
    out = args.output.resolve() / case["name"]
    out.mkdir(parents=True, exist_ok=True)
    _run(
        [
            args.yosys,
            "-p",
            f"read_verilog {fixture / 'top.v'}; "
            f"synth_intel_alm -nobram -nolutram -nodsp -top top; "
            f"write_json {out / 'input.json'}",
        ],
        out / "synth.log",
    )
    command = [args.nextpnr, "--device", "5CSEBA6U23I7"]
    if case["freq"]:
        command += ["--freq", "50"]
    command += [
        "--qsf",
        fixture / "pins.qsf",
        "--sdc",
        fixture / "clocks.sdc",
        "--json",
        out / "input.json",
        "--write",
        out / "routed.json",
    ]
    _run(command, out / "route.log")
    routed = out / "routed.json"
    cells = json.loads(routed.read_text())["modules"]["top"]["cells"]
    packed = [cell for cell in cells.values() if cell["type"] == case["packed"]]
    if len(packed) != 1:
        found = sorted({cell["type"] for cell in cells.values()})
        raise AssertionError(f"{case['name']}: expected one {case['packed']}, found {found}")
    if not clock_route_ok(case["route"], "", routed):
        details = "\n".join(_clock_routings(routed)) or "(no DCMUX in any routed net)"
        raise AssertionError(
            f"{case['name']}: {case['route']} did not reach that pad's DCMUX\n{details}"
        )
    print(f"PASS: {case['name']} routed clock reaches {case['route']}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--yosys", type=Path, required=True)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.resolve().mkdir(parents=True, exist_ok=True)
    for case in _CASES:
        _route_case(args, case)
    print("PASS: routed pad clocks match the pad-to-DCMUX table")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
