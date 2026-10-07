#!/usr/bin/env python3
"""Replay issue #165's full seed-4 scaffold under its original strict fence.

This is an expected-failure connectivity regression, not a successful cart
route or timing/CRAM qualification. Supply the retained golden input directory.
"""

import argparse
import hashlib
from pathlib import Path
import subprocess


HASHES = {
    "scaffold.json": "c11e2f071d93de1ce351434a40f8a01a512769d118b32312d51aff422d944a5c",
    "cart.json": "1e0e5ef1305d1ec8ae8bb1a4aeb94ac57d2926b4d6c11a8c44fdae985925c2b4",
    "cart.qsf": "461004549976f546503868b5a3e2d8968de9ed36638f75d2c6b93d53cd4a514c",
    "clocks.sdc": "6f09dade2abb7b2982d268786b8aa93bab0c1ce1c2aa650c70e39606e033f5cb",
}


def check_inputs(fixture):
    for name, expected in HASHES.items():
        actual = hashlib.sha256((fixture / name).read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f"Wrong golden fixture: {name} SHA256 {actual}, expected {expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("nextpnr", "fixture", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    if output == fixture or fixture in output.parents:
        raise SystemExit("Use an output directory outside the golden fixture")
    check_inputs(fixture)
    output.mkdir(parents=True, exist_ok=True)
    for router in ("router2", "gpu"):
        command = [
            str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
            "--json", str(fixture / "scaffold.json"),
            "--qsf", str(fixture / "cart.qsf"),
            "--sdc", str(fixture / "clocks.sdc"), "--freq", "74.25",
            "--fes-scaffold", "--fes-cart", str(fixture / "cart.json"),
            "--fes-cart-region", "video", "--fes-slot-clock", "pixel_clk",
            "--fes-cram-region", "1769,3442,2806,5162",
            "--no-pack", "--seed", "4", "--router", router,
            "--placer-heap-timingweight", "300",
            "--rbf", str(output / (router + ".rbf")), "--compress-rbf",
        ]
        log_path = output / (router + ".log")
        with log_path.open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
        text = log_path.read_text()
        expected = (
            "FES boundary net 'video_plug_request[11]' from WIRE.24.41.FFOUT[22] cannot reach GOUT.25.42.58",
            "CRAM fence 1769,3442,2806,5162",
            "FES boundary exit blocked by frozen net 'video_request[24]'",
            "FES frozen boundary cannot reach placed cart sinks",
        )
        if result.returncode != 125 or any(message not in text for message in expected):
            raise SystemExit(f"Unexpected {router} result ({result.returncode}); see {log_path}")
        check_inputs(fixture)
        print(f"PASS: {router} selection rejects occupied frozen boundary before backend routing")


if __name__ == "__main__":
    main()
