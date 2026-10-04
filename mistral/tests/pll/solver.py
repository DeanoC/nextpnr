#!/usr/bin/env python3
"""Compile and run the standalone PLL solver checks (no device database).

solver_config.cpp asserts the previously checked profiles and fail-closed
cases; solver_cases.cpp replays every compiled Quartus 17.0.2 oracle PLL in
fixtures/solver/cases.txt and compares all solver-controlled FPLL fields.
"""
import argparse
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    mistral = here.parent.parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name, extra in (("solver_config", []), ("solver_cases", [str(here / "fixtures" / "solver" / "cases.txt")])):
        exe = out / name
        subprocess.run([args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-I", str(mistral),
                        str(here / f"{name}.cpp"), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)] + extra, capture_output=True, text=True)
        print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else "")
        assert result.returncode == 0, result.stdout + result.stderr
    print("PASS: PLL solver standalone checks")


if __name__ == "__main__":
    main()
