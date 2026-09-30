#!/usr/bin/env python3
"""Check retained FES menu GPU routing, analogue timing and repeatability."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess


FIXTURE = Path(__file__).with_name("menu_ddr_seed1")


def run(args, seed, output):
    # A fresh directory prevents old outputs from satisfying a failed run.
    output.mkdir(parents=True, exist_ok=False)
    synth = output / "synth.json"
    with gzip.open(args.fixture / "synth.json.gz", "rb") as source:
        synth.write_bytes(source.read())

    command = [
        str(args.nextpnr.resolve()), "--json", str(synth),
        "--device", "5CSEBA6U23I7", "--qsf", str(args.fixture / "constraints.qsf"),
        "--sdc", str(args.fixture / "clocks.sdc"), "--freq", "74.25",
        "--seed", str(seed), "--router", "gpu", "--gpu-device", str(args.gpu),
        "--rbf", str(output / "core.rbf"), "--compress-rbf",
        "--write", str(output / "routed.json"), "--report", str(output / "timing.json"),
        "--detailed-timing-report",
    ]
    if args.gpu_cpu:
        command.append("--gpu-cpu")
    log_path = output / "nextpnr.log"
    with log_path.open("w") as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
        except subprocess.TimeoutExpired as exc:
            raise SystemExit(f"FAIL: menu DDR GPU route timed out; see {log_path}") from exc
    text = log_path.read_text(errors="replace")
    if (result.returncode != 0 or "Routing complete." not in text
            or re.search(r"iter=\d+ [^\n]* overused=0\b", text) is None
            or not all((output / name).is_file() and (output / name).stat().st_size > 0
                       for name in ("core.rbf", "routed.json", "timing.json"))):
        raise SystemExit(f"FAIL: menu DDR seed {seed} did not produce a legal complete route; see {log_path}")
    clocks = json.loads((output / "timing.json").read_text()).get("fmax", {})
    if (not clocks or (args.expect_clock is not None and args.expect_clock not in clocks)
            or not all(math.isfinite(c["achieved"]) and math.isfinite(c["constraint"])
                       and c["constraint"] > 0 and c["achieved"] >= c["constraint"]
                       for c in clocks.values())
            or max(c["constraint"] for c in clocks.values()) < 74.25):
        raise SystemExit(f"FAIL: menu DDR seed {seed} failed final analogue timing; see {log_path}")
    checksums = re.findall(r"Info: Checksum: (0x[0-9a-f]+)", text)
    if not checksums:
        raise SystemExit(f"FAIL: menu DDR seed {seed} has no routing checksum; see {log_path}")
    return {"seed": seed, "checksum": checksums[-1],
            "rbf_sha256": hashlib.sha256((output / "core.rbf").read_bytes()).hexdigest(),
            "fmax": clocks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, default=FIXTURE,
                        help="directory containing synth.json.gz, constraints.qsf and clocks.sdc")
    parser.add_argument("--seeds", type=int, nargs="+", default=[1])
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--expect-clock", help="clock that must appear in the final timing report")
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--gpu-cpu", action="store_true")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if args.repeat < 1 or args.timeout <= 0 or any(s < 1 for s in args.seeds):
        parser.error("repeat, timeout and seeds must be positive")
    args.fixture = args.fixture.resolve()
    output = args.output.resolve()
    results = []
    seeds = list(dict.fromkeys(args.seeds))
    single_run = len(seeds) == 1 and args.repeat == 1
    for seed in seeds:
        previous = None
        for attempt in range(1, args.repeat + 1):
            destination = output if single_run else output / f"seed-{seed}-run-{attempt}"
            result = run(args, seed, destination)
            if previous is not None and result != previous:
                raise SystemExit(f"FAIL: menu DDR seed {seed} did not reproduce; see {destination}")
            previous = result
            results.append(result)
            clock = args.expect_clock or max(result["fmax"], key=lambda c: result["fmax"][c]["constraint"])
            print(f"PASS: menu DDR seed {seed} run {attempt} completed a legal GPU route "
                  f"at {result['fmax'][clock]['achieved']:.2f} MHz ({clock})")
    (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
