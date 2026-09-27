#!/usr/bin/env python3
"""Route the retained FES menu DDR seed-1 plateau with the GPU router."""

import argparse
import gzip
from pathlib import Path
import re
import subprocess


FIXTURE = Path(__file__).with_name("menu_ddr_seed1")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    synth = output / "synth.json"
    with gzip.open(FIXTURE / "synth.json.gz", "rb") as source:
        synth.write_bytes(source.read())

    command = [
        str(args.nextpnr.resolve()), "--json", str(synth),
        "--device", "5CSEBA6U23I7", "--qsf", str(FIXTURE / "constraints.qsf"),
        "--sdc", str(FIXTURE / "clocks.sdc"), "--freq", "74.25",
        "--seed", "1", "--router", "gpu", "--gpu-device", str(args.gpu),
        "--rbf", str(output / "core.rbf"), "--compress-rbf",
        "--write", str(output / "routed.json"), "--report", str(output / "timing.json"),
        "--detailed-timing-report",
    ]
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
            or not all((output / name).is_file() for name in ("core.rbf", "routed.json", "timing.json"))):
        raise SystemExit(f"FAIL: menu DDR seed 1 did not produce a legal complete route; see {log_path}")
    print("PASS: menu DDR seed 1 completed a legal GPU route")


if __name__ == "__main__":
    main()
