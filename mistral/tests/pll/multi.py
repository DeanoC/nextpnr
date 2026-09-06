#!/usr/bin/env python3
"""Check common-tuple triple/quad PLL selection against Quartus; host only."""
import argparse
import copy
import json
from pathlib import Path
import sys

from check import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--skip-negative", action="store_true")
    parser.add_argument("--profile", choices=("tripleDecimal", "triple400", "quad320", "quad400"),
                        help="run one reference profile instead of the full matrix")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # The first two triple outputs fit 300 MHz, but the third requires 400.
    # Decimal spellings must retain exact values through all output parsers.
    profiles = (
        ("triple", "tripleDecimal", ("12.5", "25.00", "50.0")),
        ("triple", "triple400", ("25.000000", "50.0", "80.000")),
        ("quad", "quad320", ("40.000", "80.0", "16.000000", "32.00")),
        # The first three outputs fit 300 MHz; the fourth forces 400.
        ("quad", "quad400", ("25.000", "50.0", "100.000", "80.00")),
    )
    for kind, profile, frequencies in profiles:
        if args.profile and args.profile != profile:
            continue
        profile_out = out / profile
        profile_out.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(fixture / f"{kind}.py"),
                   "--yosys", str(args.yosys.resolve()),
                   "--nextpnr", str(args.nextpnr.resolve()),
                   "--mistral-cv", str(args.mistral_cv.resolve()),
                   "--output", str(profile_out),
                   "--oracle-fixture", str(fixture / "fixtures" / "multi" / profile),
                   "--frequencies", *frequencies]
        if args.skip_negative:
            command.append("--skip-negative")
        run(command, profile_out / "check.log")
        if args.skip_negative:
            continue
        design = json.loads((profile_out / "synth.json").read_text())
        route = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                 "--qsf", str(fixture / "diagnostic.qsf"),
                 "--sdc", str(profile_out / "clocks.sdc"), "--freq", "50"]

        def reject(name, values):
            invalid = copy.deepcopy(design)
            params = invalid["modules"]["top"]["cells"]["pll"]["parameters"]
            for index, value in values.items():
                params[f"output_clock_frequency{index}"] = f"{value} MHz"
            path = profile_out / f"invalid-{name}.json"
            path.write_text(json.dumps(invalid))
            log = run(route + ["--json", str(path)],
                      profile_out / f"invalid-{name}.log", success=False)
            assert "ERROR" in log and any(reason in log for reason in
                                          ("frequen", "tuple", "profile")), log

        for index in range(len(frequencies)):
            for label, value in (("nondivisor", "7"),
                                 ("inexact", frequencies[index] + "1"),
                                 ("below-range", "0.5"),
                                 ("above-range", "101")):
                reject(f"{label}-{index}", {index: value})
        # Each requested output is individually supported, but they do not
        # share one checked tuple. A per-output solver must not accept them.
        reject("no-common-tuple", {0: "75", 1: "80"})
    print("PASS: multi-output PLL common-tuple selection and full FPLL oracles")


if __name__ == "__main__":
    main()
