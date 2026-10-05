#!/usr/bin/env python3
"""Route general-solver PLL designs and compare every FPLL setting with Quartus.

Each fixtures/general/<case> bundle holds the design, the pins/SDC used by
both tools, nextpnr's placement (sites and output counters) that the Quartus
project pins, the compressed Quartus RBF and its decoded FPLL/CMUX settings.
"""
import argparse
import gzip
import hashlib
import json
import re
import subprocess
from pathlib import Path

from check import run

CASES = {
    # case: {clock net: expected MHz}
    "mister3": {"hdmi_clk": 148.5, "audio_clk": 24.576, "core_clk[0]": 189.0, "core_clk[1]": 85.909090,
                "core_clk[2]": 21.477272},
    "eight": {"pll8_clk[%d]" % i: f for i, f in enumerate((150, 120, 100, 75, 60, 50, 40, 30))},
    "oddn": {"pllodd_clk[0]": 40, "pllodd_clk[1]": 80, "pllodd_clk[2]": 16, "pllodd_clk[3]": 32},
    "refs": {"pll27_clk[0]": 74.25, "pll27_clk[1]": 148.5, "pll100_clk": 27.0, "pll25_clk": 250.0},
}


def fpll_lines(text, sites):
    keep = set()
    for line in text.splitlines():
        m = re.match(r"^([si]) (FPLL\.\d+\.\d+):", line)
        if m and (m.group(2) in sites or line.startswith("s FPLL.000.073:PL_AUX_BG_POWERDOWN")):
            keep.add(line)
    return keep


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--case", action="append", choices=sorted(CASES))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent / "fixtures" / "general"
    out_root = args.output.resolve()
    for case in args.case or sorted(CASES):
        fixture = root / case
        out = out_root / case
        out.mkdir(parents=True, exist_ok=True)
        sums = dict(reversed(line.split()) for line in (fixture / "sha256.txt").read_text().splitlines())
        packed = (fixture / "top.rbf.gz").read_bytes()
        assert hashlib.sha256(packed).hexdigest() == sums["top.rbf.gz"], case
        raw = gzip.decompress(packed)
        assert hashlib.sha256(raw).hexdigest() == sums["top.rbf"], case
        (out / "quartus.rbf").write_bytes(raw)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "quartus.rbf"),
             str(out / "quartus.bt")], out / "quartus-decomp.log")
        placement = {}
        for line in (fixture / "placement.txt").read_text().splitlines():
            name, x, y, counters = line.split()
            placement[name] = ("FPLL.%03d.%03d" % (int(x), int(y)), counters)
        sites = {site for site, _ in placement.values()}
        quartus = fpll_lines((out / "quartus.bt").read_text(), sites)
        assert quartus == fpll_lines((fixture / "pll-settings.txt").read_text(), sites), case

        run([str(args.yosys.resolve()), "-p", f'read_verilog "{fixture / "top.v"}"; '
             'synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{out / "synth.json"}"'], out / "yosys.log")
        log = run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7", "--qsf", str(fixture / "pins.qsf"),
                   "--sdc", str(fixture / "clocks.sdc"), "--json", str(out / "synth.json"),
                   "--rbf", str(out / "top.rbf"), "--compress-rbf", "--report", str(out / "timing.json")],
                  out / "route.log")
        for name, (site, counters) in placement.items():
            x, y = int(site[5:8]), int(site[9:12])
            pattern = r"PLL '%s': .* counters C%s, bel altera_pll\.%d\.%d\.\d+" % (re.escape(name), counters, x, y)
            assert re.search(pattern, log), (case, name, "placement differs from the Quartus-pinned oracle")
        report = json.loads((out / "timing.json").read_text())
        assert report["utilization"]["altera_pll"]["used"] == len(placement)
        for clock, mhz in CASES[case].items():
            constraint = report["fmax"][clock]["constraint"]
            # Constraints are quantised to whole picoseconds.
            assert abs(1e6 / constraint - 1e6 / mhz) <= 1.0, (case, clock, constraint, mhz)
            assert report["fmax"][clock]["achieved"] >= constraint, (case, clock)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "top.rbf"), str(out / "top.bt")],
            out / "decomp.log")
        ours = fpll_lines((out / "top.bt").read_text(), sites)
        if ours != quartus:
            raise AssertionError("%s FPLL settings differ\n  quartus only: %s\n  nextpnr only: %s" % (
                case, sorted(quartus - ours), sorted(ours - quartus)))
        print("PASS %s: %d PLLs, %d FPLL settings identical to Quartus; RBF sha256 %s" % (
            case, len(placement), len(ours), hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest()))


if __name__ == "__main__":
    main()
