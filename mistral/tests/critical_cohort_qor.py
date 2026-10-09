#!/usr/bin/env python3
"""Compare cohort repair on one unchanged synthesis JSON and constraint set.

Every route starts from synthesis: a placed JSON without external IO delays
does not serialize the generated PLL clock constraints. Process success with
--timing-allow-fail is distinct from timing acceptance in the summary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "json", "qsf", "sdc", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--seeds", nargs="+", type=int, default=[2, 3])
    parser.add_argument("--budget", type=int, default=64)
    parser.add_argument("--device", default="5CSEBA6U23I7")
    parser.add_argument("--freq", type=float, default=74.25)
    parser.add_argument("--route", action="store_true")
    parser.add_argument("--guided", action="store_true", help="use each seed's fresh baseline route as repair guidance")
    parser.add_argument("--calibrated", action="store_true", help="guide with measured data-arc calibration from each fresh baseline")
    parser.add_argument("--gpu-device", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if not 1 <= args.budget <= 64 or args.timeout <= 0:
        parser.error("budget must be 1..64 and timeout must be positive")
    if args.calibrated:
        args.guided = True
    if args.guided and not args.route:
        parser.error("--guided requires --route")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    summary = {"inputs": {}, "route": args.route, "guided": args.guided, "calibrated": args.calibrated, "runs": []}
    for name in ("json", "qsf", "sdc"):
        path = getattr(args, name).resolve()
        summary["inputs"][name] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    base = [str(args.nextpnr.resolve()), "--device", args.device, "--json", str(args.json.resolve()),
            "--qsf", str(args.qsf.resolve()), "--sdc", str(args.sdc.resolve()), "--freq", str(args.freq),
            "--placer", "heap", "--placer-heap-timingweight", "2000", "--placer-heap-critexp", "5",
            "--timing-allow-fail"]
    for seed in args.seeds:
        for budget in (0, args.budget):
            tag = f"s{seed}-b{budget}"
            report = out / (tag + "-timing.json")
            command = base + ["--seed", str(seed), "--critical-cohort-budget", str(budget),
                              "--write", str(out / (tag + ".json")), "--report", str(report)]
            if args.route:
                command += ["--router", "gpu", "--gpu-device", str(args.gpu_device),
                            "--rbf", str(out / (tag + ".rbf"))]
            else:
                command += ["--no-route"]
            if args.calibrated:
                command += ["--timing-report-paths", "16"]
                if not budget:
                    command += ["--critical-cohort-model-out", str(out / f"s{seed}-model.json")]
            if args.guided and budget:
                suffix = "model.json" if args.calibrated else "b0-timing.json"
                command += ["--critical-cohort-report", str(out / f"s{seed}-{suffix}")]
            # A timeout must not leave a previous report looking like this run.
            report.unlink(missing_ok=True)
            if args.calibrated and not budget:
                (out / f"s{seed}-model.json").unlink(missing_ok=True)
            row = {"seed": seed, "budget": budget, "command": command}
            start = time.monotonic()
            with (out / (tag + ".log")).open("w") as log:
                try:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                    row["exit_code"] = result.returncode
                except subprocess.TimeoutExpired:
                    row["exit_code"] = "timeout"
            row["wall_seconds"] = time.monotonic() - start
            row["timing_pass"] = False
            if report.exists() and row["exit_code"] == 0:
                data = json.loads(report.read_text())
                row["fmax"] = data.get("fmax", {})
                row["timing_summary"] = data.get("timing_summary", {})
                clocks = row["fmax"]
                row["timing_pass"] = bool(clocks) and all(
                    clock["achieved"] >= clock["constraint"] for clock in clocks.values())
                margins = row["timing_summary"].get("clocks", {})
                row["timing_pass"] &= bool(margins) and all(
                    clock["setup_wns_ns"] >= 0 and clock["hold_wns_ns"] >= 0 for clock in margins.values())
                if args.route:
                    row["timing_pass"] &= row["timing_summary"].get("final_analogue_model", False)
            text = (out / (tag + ".log")).read_text()
            match = re.search(r"Critical cohort: attempted=(\d+)/(\d+) nodes=(\d+)/(\d+) timed=(\d+)/(\d+) kept=(\d+)", text)
            if match:
                row["repair"] = dict(zip(("attempted", "attempt_limit", "nodes", "node_limit", "timed", "timing_limit", "kept"),
                                         map(int, match.groups())))
            summary["runs"].append(row)
            (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            print(tag, row["exit_code"], "timing_pass=" + str(row["timing_pass"]), flush=True)


if __name__ == "__main__":
    main()
