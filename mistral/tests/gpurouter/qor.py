#!/usr/bin/env python3
"""Compare the GPU router against a reference router on a Mistral fixture.

For every requested seed the fixture is routed with ``--router gpu`` (twice,
to check that the routing checksum reproduces) and with the reference router
(``router2`` by default, ``--reference router1`` or ``none``). Every run
writes a bitstream so that the final report carries Mistral's analogue
interconnect timing, which is what signoff uses; the per-pip delay-table
Fmax the router itself optimises is reported next to it. The script checks
that each run converged, produced a checksum and a non-empty set of timing
clocks (``--expect-clock`` names clocks that must be present), passed
signoff (or, with ``--timing-allow-fail``, merely reports it) and reproduces,
and prints router time and both Fmax views side by side. It needs only a
built ``nextpnr-mistral``; no Yosys, Quartus or board.

Example, using the retained mixed-width M10K netlist:

    python3 mistral/tests/gpurouter/qor.py \\
        --nextpnr build/nextpnr-mistral --output /tmp/gpurouter-m10k

Or an external fixture such as the misteross FES ZX81 synthesis output, with
the placer settings its recipe uses:

    python3 mistral/tests/gpurouter/qor.py --nextpnr build/nextpnr-mistral \\
        --fixture fes-zx81-oss/synth.json --qsf constraints-oss.qsf \\
        --sdc clocks-oss.sdc --freq 74.25 --timing-allow-fail \\
        --extra-arg=--placer-heap-timingweight=1000 --extra-arg=--placer-heap-critexp=5 \\
        --output /tmp/gpurouter-zx81

``--arc-dump`` additionally writes every routed arc's per-hop delay-table and
analogue delays of the first GPU run (``arcs.tsv`` next to its log), which is
how the table entries in mistral/delay.cc were checked against the model.
"""

import argparse
import gzip
import json
import os
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
TABLE_FMAX = re.compile(r"Routed Fmax \(pip delay table\) for clock '([^']+)': ([0-9.]+) MHz")
ANALOGUE_LINE = re.compile(r"Analogue signoff (check|repair round \d+): worst clock slack (-?[0-9.]+) ns")
CANDIDATE_LINE = re.compile(r"analogue candidate selection: .* (\d+) nets re-routed")


def run(args, router, seed, output, extra=(), arc_dump=False):
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
    if not args.no_rbf:
        command += ["--rbf", str(output / "out.rbf"), "--compress-rbf"]
    command += list(args.extra_arg) + list(extra)
    env = dict(os.environ)
    if arc_dump:
        env["NEXTPNR_MISTRAL_ARC_DUMP"] = str(output / "arcs.tsv")
    log_path = output / "route.log"
    started = time.monotonic()
    with log_path.open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=args.timeout, env=env)
    wall = time.monotonic() - started
    text = log_path.read_text()
    m = ROUTER_TIME[router].search(text)
    router_time = float(m.group(1)) if m and m.lastindex else None
    checksums = CHECKSUM.findall(text)
    data = json.loads(report.read_text())
    fmax = {clock: (r["achieved"], r["constraint"]) for clock, r in data.get("fmax", {}).items()}
    # The last table-model line describes the routing the report was made from
    table = {}
    for clock, achieved in TABLE_FMAX.findall(text):
        table[clock] = float(achieved)
    analogue_rounds = [(kind, float(slack)) for kind, slack in ANALOGUE_LINE.findall(text)]
    candidates = sum(int(n) for n in CANDIDATE_LINE.findall(text))
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
            "fmax": fmax, "table": table, "analogue_rounds": analogue_rounds, "candidates": candidates,
            "retried": retried, "log": log_path}


def check_evidence(args, label, result, failures):
    if result["checksum"] is None:
        failures.append(f"{label}: no routing checksum in {result['log']}")
    if not result["fmax"]:
        failures.append(f"{label}: the timing report names no clocks")
    for clock in args.expect_clock:
        if clock not in result["fmax"]:
            failures.append(f"{label}: clock '{clock}' missing from the timing report")
    if not args.no_rbf and not result["analogue_rounds"] and not args.no_reference_analogue_check:
        pass  # only the GPU flow logs analogue rounds; the report itself is post-bitstream


def fmt_fmax(result):
    parts = []
    for clock, (achieved, constraint) in result["fmax"].items():
        table = result["table"].get(clock)
        parts.append(f"{clock}={achieved:.2f}/{constraint:.2f} MHz" + (f" (table {table:.2f})" if table else ""))
    return ", ".join(parts)


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
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--timing-allow-fail", action="store_true")
    parser.add_argument("--no-rbf", action="store_true",
                        help="report the per-pip delay table instead of the post-bitstream analogue model")
    parser.add_argument("--reference", choices=["router2", "router1", "none"], default="router2")
    parser.add_argument("--no-reference", action="store_true", help="same as --reference none")
    parser.add_argument("--expect-clock", action="append", default=[], help="clock that must appear in every report")
    parser.add_argument("--extra-arg", action="append", default=[], help="extra nextpnr argument for every run")
    parser.add_argument("--gpu-arg", action="append", default=[], help="extra argument for the gpu runs")
    parser.add_argument("--arc-dump", action="store_true", help="write the per-hop analogue arc dump of the GPU run")
    parser.add_argument("--no-reference-analogue-check", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.no_reference:
        args.reference = "none"
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

    model = "table model (no bitstream)" if args.no_rbf else "post-bitstream analogue model"
    print(f"reported Fmax: {model}; table-model Fmax of the same routing in parentheses", flush=True)
    failures = []
    for seed in args.seed or [1, 2]:
        gpu = run(args, "gpu", seed, out / f"seed{seed}" / "gpu", args.gpu_arg, arc_dump=args.arc_dump)
        gpu2 = run(args, "gpu", seed, out / f"seed{seed}" / "gpu-repeat", args.gpu_arg)
        check_evidence(args, f"seed {seed} gpu", gpu, failures)
        check_evidence(args, f"seed {seed} gpu-repeat", gpu2, failures)
        if gpu["checksum"] != gpu2["checksum"]:
            failures.append(f"seed {seed}: GPU routing is not reproducible ({gpu['checksum']} vs {gpu2['checksum']})")
        for clock, (achieved, constraint) in gpu["fmax"].items():
            if achieved < constraint and not args.timing_allow_fail:
                failures.append(f"seed {seed}: gpu {clock} {achieved:.2f} MHz < {constraint:.2f} MHz")
        ref = None
        if args.reference != "none":
            ref = run(args, args.reference, seed, out / f"seed{seed}" / args.reference)
            check_evidence(args, f"seed {seed} {args.reference}", ref, failures)
        line = f"seed {seed}: gpu {gpu['router_time']}s (wall {gpu['wall']:.1f}s) checksum {gpu['checksum']} "
        line += fmt_fmax(gpu)
        if gpu["analogue_rounds"]:
            rounds = ", ".join(f"{k} {s:+.3f} ns" for k, s in gpu["analogue_rounds"])
            line += f" [analogue rounds: {rounds}; {gpu['candidates']} nets by candidate selection]"
        if ref is not None:
            if ref["retried"] is None:
                line += f" | {args.reference} {ref['router_time']}s (wall {ref['wall']:.1f}s) " + fmt_fmax(ref)
            else:
                own = ref["retried"]["router2_fmax"]
                line += f" | router2 {ref['router_time']}s " + ", ".join(f"{c}={a:.2f} MHz" for c, a in own.items())
                line += " (Mistral retried with router1; final wall {:.1f}s ".format(ref["wall"]) + fmt_fmax(ref) + ")"
        print(line, flush=True)
    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures))
    print("PASS")


if __name__ == "__main__":
    main()
