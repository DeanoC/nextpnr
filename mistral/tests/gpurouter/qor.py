#!/usr/bin/env python3
"""Compare the GPU router against router2 on a Mistral fixture.

For every requested seed the fixture is routed once with ``--router router2``
and once with ``--router gpu``. The script checks that the GPU route
converges, passes signoff timing (or, with ``--timing-allow-fail``, merely
reports it), that a second GPU run with the same seed reproduces the same
routing checksum, and prints the router time and Fmax of both routers side
by side. It needs only a built ``nextpnr-mistral``; no Yosys, Quartus or
board.

Example, using the retained mixed-width M10K netlist:

    python3 mistral/tests/gpurouter/qor.py \\
        --nextpnr build/nextpnr-mistral --output /tmp/gpurouter-m10k

Or an external fixture such as the misteross FES ZX81 synthesis output:

    python3 mistral/tests/gpurouter/qor.py --nextpnr build/nextpnr-mistral \\
        --fixture fes-zx81-oss/synth.json --qsf constraints-oss.qsf \\
        --sdc clocks-oss.sdc --freq 74.25 --output /tmp/gpurouter-zx81
"""

import argparse
import gzip
import json
import re
from pathlib import Path
import subprocess
import time

ROUTER_TIME = {
    "router2": re.compile(r"Router2 time ([0-9.]+)s"),
    "router1": re.compile(r"Info: Routing complete.*"),
    "gpu": re.compile(r"GPU router time ([0-9.]+)s"),
}
CHECKSUM = re.compile(r"Info: Checksum: (0x[0-9a-f]+)")
RETRY = "retrying with router1"
FMAX_LINE = re.compile(r"Max frequency for clock +'([^']+)': ([0-9.]+) MHz")


def run(args, router, seed, output, extra=()):
    output.mkdir(parents=True, exist_ok=True)
    report = output / "timing.json"
    command = [
        str(args.nextpnr.resolve()), "--device", args.device, "--seed", str(seed), "--router", router,
        "--json", str(args.fixture.resolve()), "--qsf", str(args.qsf.resolve()),
        "--report", str(report), "--write", str(output / "routed.json"),
    ]
    if args.sdc is not None:
        command += ["--sdc", str(args.sdc.resolve())]
    if args.freq is not None:
        command += ["--freq", str(args.freq)]
    if args.timing_allow_fail:
        command.append("--timing-allow-fail")
    command += list(extra)
    log_path = output / "route.log"
    started = time.monotonic()
    with log_path.open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=args.timeout)
    wall = time.monotonic() - started
    text = log_path.read_text()
    m = ROUTER_TIME[router].search(text)
    router_time = float(m.group(1)) if m and m.lastindex else None
    checksums = CHECKSUM.findall(text)
    data = json.loads(report.read_text())
    fmax = {clock: (r["achieved"], r["constraint"]) for clock, r in data.get("fmax", {}).items()}
    # Mistral retries a marginal router2 result with router1; the report then
    # describes router1's routing, so keep router2's own numbers separately.
    retried = None
    if router == "router2" and RETRY in text:
        before = text.split(RETRY, 1)[0]
        own = {}
        for clock, achieved in FMAX_LINE.findall(before):
            own[clock] = float(achieved)
        retried = {"final_router": "router1", "router2_fmax": own}
    return {"wall": wall, "router_time": router_time, "checksum": checksums[-1] if checksums else None,
            "fmax": fmax, "retried": retried}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, help="synthesised JSON; default: the retained M10K netlist")
    parser.add_argument("--qsf", type=Path)
    parser.add_argument("--sdc", type=Path)
    parser.add_argument("--freq", type=float)
    parser.add_argument("--device", default="5CSEBA6U23I7")
    parser.add_argument("--seed", type=int, action="append")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--timing-allow-fail", action="store_true")
    parser.add_argument("--no-reference", action="store_true", help="skip the router2 runs")
    parser.add_argument("--gpu-arg", action="append", default=[], help="extra argument for the gpu runs")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.fixture is None:
        fixture = Path(__file__).resolve().parent.parent / "router2" / "m10k-mixed.json.gz"
        args.fixture = out / "synth.json"
        args.fixture.write_bytes(gzip.decompress(fixture.read_bytes()))
        if args.qsf is None:
            args.qsf = out / "pins.qsf"
            args.qsf.write_text("set_location_assignment PIN_V11 -to FPGA_CLK1_50\n")
        if args.sdc is None:
            args.sdc = out / "clocks.sdc"
            args.sdc.write_text("create_clock -name FPGA_CLK1_50 -period 20.000 [get_ports {FPGA_CLK1_50}]\n")
    if args.qsf is None:
        parser.error("--qsf is required with --fixture")

    failures = []
    for seed in args.seed or [1, 2]:
        gpu = run(args, "gpu", seed, out / f"seed{seed}" / "gpu", args.gpu_arg)
        gpu2 = run(args, "gpu", seed, out / f"seed{seed}" / "gpu-repeat", args.gpu_arg)
        if gpu["checksum"] != gpu2["checksum"]:
            failures.append(f"seed {seed}: GPU routing is not reproducible ({gpu['checksum']} vs {gpu2['checksum']})")
        for clock, (achieved, constraint) in gpu["fmax"].items():
            if achieved < constraint and not args.timing_allow_fail:
                failures.append(f"seed {seed}: gpu {clock} {achieved:.2f} MHz < {constraint:.2f} MHz")
        ref = None if args.no_reference else run(args, "router2", seed, out / f"seed{seed}" / "router2")
        line = f"seed {seed}: gpu {gpu['router_time']}s (wall {gpu['wall']:.1f}s) checksum {gpu['checksum']}"
        line += " " + ", ".join(f"{c}={a:.2f}/{k:.2f} MHz" for c, (a, k) in gpu["fmax"].items())
        if ref is not None:
            if ref["retried"] is None:
                line += f" | router2 {ref['router_time']}s (wall {ref['wall']:.1f}s) "
                line += ", ".join(f"{c}={a:.2f} MHz" for c, (a, k) in ref["fmax"].items())
            else:
                own = ref["retried"]["router2_fmax"]
                line += f" | router2 {ref['router_time']}s " + ", ".join(f"{c}={a:.2f} MHz" for c, a in own.items())
                line += " (Mistral retried with router1; final wall {:.1f}s ".format(ref["wall"])
                line += ", ".join(f"{c}={a:.2f} MHz" for c, (a, k) in ref["fmax"].items()) + ")"
        print(line, flush=True)
    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("PASS")


if __name__ == "__main__":
    main()
