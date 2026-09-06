#!/usr/bin/env python3
"""Check triple/quad exact duties against Quartus and both timing edge budgets."""
import argparse
import copy
import json
from pathlib import Path
import re
import sys

from check import run
from triple import fpll_settings


# At 300 MHz, 25 MHz/25% is representable (C=12, high=3), but
# 100 MHz/25% is not (C=3). Thus triple400 and quad400 specifically
# require the third/fourth output duty to participate in tuple selection.
PROFILES = (
    ("triple400", (25, 50, 100), (25, 50, 25)),
    ("quad400", (25, 50, 50, 100), (25, 50, 50, 25)),
    ("quad320", (40, 80, 16, 20), (25, 75, 25, 75)),
)


def mux_settings(text):
    return dict(re.findall(r"^s (CMUXHG\.000\.035:(?:INPUT_SEL|TESTSYN_ENOUT_SELECT)\.\d+) (.+)$", text, re.M))


def edge_source(fixture, count, reverse):
    """Use observable independent launch/capture pairs on every PLL output."""
    source = (fixture / ("triple.v" if count == 3 else "quad.v")).read_text()
    source = re.sub(r"    reg .*?    altera_pll", "    altera_pll", source, flags=re.S)
    launch, capture = ("negedge", "posedge") if reverse else ("posedge", "negedge")
    registers = []
    for index in range(count):
        registers += [f"    reg launch{index} = 0, capture{index} = 0;",
                      f"    always @({launch} clocks[{index}]) launch{index} <= gpo[{index}];",
                      f"    always @({capture} clocks[{index}]) capture{index} <= launch{index};"]
    source = source.replace("    altera_pll", "\n".join(registers) + "\n    altera_pll", 1)
    captures = ", ".join(f"capture{i}" for i in reversed(range(count)))
    return re.sub(r"\.gp_in\(\{.*?\}\)", f".gp_in({{{31-count}'b0, locked, {captures}}})", source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name, frequencies, duties in PROFILES:
        case = out / name
        case.mkdir(exist_ok=True)
        count = len(frequencies)
        runner = fixture / ("triple.py" if count == 3 else "quad.py")
        run([sys.executable, str(runner), "--yosys", str(args.yosys.resolve()),
             "--nextpnr", str(args.nextpnr.resolve()), "--mistral-cv", str(args.mistral_cv.resolve()),
             "--output", str(case), "--frequencies", *map(str, frequencies),
             "--duties", *map(str, duties), "--oracle-fixture",
             str(fixture / "fixtures" / "multi-duty" / name), "--skip-negative"], case / "check.log")
        oracle = (case / "quartus.bt").read_text()
        baseline = (case / "top.bt").read_text()
        command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                   "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(case / "clocks.sdc"),
                   "--freq", "50", "--compress-rbf"]
        design = json.loads((case / "synth.json").read_text())
        for index in range(count):
            for duty in (0, 100, 1):
                invalid = copy.deepcopy(design)
                invalid["modules"]["top"]["cells"]["pll"]["parameters"][f"duty_cycle{index}"] = format(duty, "032b")
                path = case / f"invalid-duty{index}-{duty}.json"
                path.write_text(json.dumps(invalid))
                log = run(command + ["--json", str(path)], path.with_suffix(".log"), success=False)
                reason = "integer percent from 1 to 99" if duty in (0, 100) else "frequencies/duties"
                assert "ERROR" in log and reason in log, log
        for reverse in (False, True):
            edge = case / ("fall-rise" if reverse else "rise-fall")
            edge.mkdir(exist_ok=True)
            source = edge / "top.v"
            source.write_text(edge_source(fixture, count, reverse))
            run([str(args.yosys.resolve()), "-p", f'read_verilog "{source}"; '
                 'synth_intel_alm -nobram -nolutram -nodsp -top top; '
                 f'write_json "{edge / "synth.json"}"'], edge / "yosys.log")
            edge_design = json.loads((edge / "synth.json").read_text())
            edge_design["modules"]["top"]["cells"]["pll"]["parameters"] = copy.deepcopy(
                design["modules"]["top"]["cells"]["pll"]["parameters"])
            (edge / "synth.json").write_text(json.dumps(edge_design))
            run(command + ["--json", str(edge / "synth.json"), "--rbf", str(edge / "top.rbf"),
                           "--report", str(edge / "timing.json")], edge / "route.log")
            report = json.loads((edge / "timing.json").read_text())
            start, end = ("negedge", "posedge") if reverse else ("posedge", "negedge")
            for index, (frequency, duty) in enumerate(zip(frequencies, duties)):
                clock = f"clocks[{index}]"
                paths = [p for p in report["critical_paths"]
                         if p["from"] == start + " " + clock and p["to"] == end + " " + clock]
                assert len(paths) == 1, report["critical_paths"]
                fraction = (100 - duty if reverse else duty) / 100
                budget = 1000 / frequency * fraction
                assert abs(paths[0]["max_delay"] - budget) < 0.002, paths[0]
                fmax = report["fmax"][clock]
                assert abs(fmax["constraint"] - frequency) < 0.005 and fmax["achieved"] >= frequency, fmax
                expected_fmax = 1000 * fraction / sum(p["delay"] for p in paths[0]["path"])
                assert abs(fmax["achieved"] - expected_fmax) < expected_fmax * 0.0001, (fmax, expected_fmax)
            run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
                 str(edge / "top.rbf"), str(edge / "top.bt")], edge / "decomp.log")
            bt = (edge / "top.bt").read_text()
            assert fpll_settings(bt) == fpll_settings(oracle)
            assert mux_settings(bt) == mux_settings(baseline)
        print(f"PASS: {name}: all FPLLs, muxes, duties, invalid outputs and both edge budgets (host only)")


if __name__ == "__main__":
    main()
