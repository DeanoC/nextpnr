#!/usr/bin/env python3
"""Check zero-phase multi-output PLLs at 25/100 MHz references; host checks only."""
import argparse
import copy
import json
from pathlib import Path
import sys

from check import run


# Together these require each checked feedback tuple. The third-output duty
# forces triple400 past the otherwise sufficient 300 MHz configuration.
PROFILES = (
    ("triple300", (25, 50, 100), (50, 50, 50)),
    ("triple400", (25, 50, 100), (25, 50, 25)),
    ("quad320", (40, 80, 16, 20), (25, 75, 25, 75)),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for reference in (25, 100):
        for profile, frequencies, duties in PROFILES:
            name = f"ref{reference}-{profile}"
            case = out / name
            case.mkdir(exist_ok=True)
            count = len(frequencies)
            runner = fixture / ("triple.py" if count == 3 else "quad.py")
            # Run the complete existing negative suite as well as oracle,
            # utilization, physical mux and generated-clock checks.
            run([sys.executable, str(runner), "--yosys", str(args.yosys.resolve()),
                 "--nextpnr", str(args.nextpnr.resolve()), "--mistral-cv", str(args.mistral_cv.resolve()),
                 "--output", str(case), "--reference-mhz", str(reference),
                 "--frequencies", *map(str, frequencies), "--duties", *map(str, duties),
                 "--oracle-fixture", str(fixture / "fixtures" / "multi-reference" / name)], case / "check.log")
            command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                       "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(case / "clocks.sdc"),
                       "--freq", str(reference), "--compress-rbf"]
            design = json.loads((case / "synth.json").read_text())
            mismatched = case / "mismatched-reference.sdc"
            mismatched.write_text("create_clock -name FPGA_CLK1_50 -period 20 "
                                  "[get_ports {FPGA_CLK1_50}]\n")
            invalid_command = command.copy()
            invalid_command[invalid_command.index("--sdc") + 1] = str(mismatched)
            log = run(invalid_command + ["--json", str(case / "synth.json")],
                      case / "invalid-reference-sdc.log", success=False)
            assert "ERROR" in log and "conflicting clock constraint" in log, log
            for index in range(1, count):
                invalid = copy.deepcopy(design)
                params = invalid["modules"]["top"]["cells"]["pll"]["parameters"]
                for output in range(count):
                    params[f"output_clock_frequency{output}"] = "25 MHz"
                    params[f"duty_cycle{output}"] = format(50, "032b")
                    params[f"phase_shift{output}"] = "0 ps"
                params[f"phase_shift{index}"] = "10000 ps"
                path = case / f"invalid-reference-phase{index}.json"
                path.write_text(json.dumps(invalid))
                log = run(command + ["--json", str(path)], path.with_suffix(".log"), success=False)
                assert "ERROR" in log and "phase" in log and "50 MHz reference" in log, log
            print(f"PASS: {name}: all FPLLs, output clocks, utilization and rejection checks (host only)")


if __name__ == "__main__":
    main()
