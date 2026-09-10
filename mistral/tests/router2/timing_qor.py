#!/usr/bin/env python3
"""Run a multi-clock Mistral fixture through router2 and check signoff timing.

The fixture is supplied by the caller so this regression can use a generated
design without adding a large synthesis artifact to the nextpnr repository.
"""

import argparse
import json
from pathlib import Path
import subprocess
import time


def run_case(args, seed, output):
    output.mkdir(parents=True, exist_ok=True)
    report = output / "timing.json"
    command = [
        str(args.nextpnr.resolve()),
        "--device",
        args.device,
        "--seed",
        str(seed),
        "--router",
        args.router,
        "--json",
        str(args.fixture.resolve()),
        "--qsf",
        str(args.qsf.resolve()),
        "--sdc",
        str(args.sdc.resolve()),
        "--report",
        str(report),
        "--write",
        str(output / "routed.json"),
        "--compress-rbf",
        "--rbf",
        str(output / "top.rbf"),
    ]
    if args.timing_allow_fail:
        command.append("--timing-allow-fail")
    log_path = output / "route.log"
    started = time.monotonic()
    with log_path.open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=args.timeout)
    report_data = json.loads(report.read_text())
    fmax = report_data.get("fmax", {})
    if not fmax:
        raise AssertionError(f"seed {seed} has no constrained clocks")
    utilization = report_data.get("utilization", {})
    if utilization.get("altera_pll", {}).get("used", 0) < args.min_pll:
        raise AssertionError(f"seed {seed} does not exercise {args.min_pll} PLLs")
    if utilization.get("MISTRAL_M10K", {}).get("used", 0) < args.min_m10k:
        raise AssertionError(f"seed {seed} does not exercise {args.min_m10k} M10Ks")
    failures = []
    for clock, result in fmax.items():
        achieved = result.get("achieved")
        constraint = result.get("constraint")
        if achieved is None or constraint is None or achieved + args.tolerance < constraint:
            failures.append(f"{clock}: {achieved} MHz < {constraint} MHz")
    if failures:
        raise AssertionError(f"seed {seed} failed signoff timing: {', '.join(failures)}")
    if not (output / "top.rbf").is_file() or (output / "top.rbf").stat().st_size == 0:
        raise AssertionError(f"seed {seed} did not produce a compressed RBF")
    elapsed = time.monotonic() - started
    rates = ", ".join(f"{clock}={result['achieved']:.3f} MHz" for clock, result in fmax.items())
    print(f"PASS: seed {seed}: {rates} ({elapsed:.1f}s)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--qsf", type=Path, required=True)
    parser.add_argument("--sdc", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", default="5CSEBA6U23I7")
    parser.add_argument("--router", default="router2")
    parser.add_argument("--seed", type=int, action="append")
    parser.add_argument("--min-pll", type=int, default=2)
    parser.add_argument("--min-m10k", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--tolerance", type=float, default=0.0)
    parser.add_argument("--timing-allow-fail", action="store_true")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    for seed in args.seed or [1, 3, 7]:
        run_case(args, seed, args.output.resolve() / f"seed{seed}")


if __name__ == "__main__":
    main()
