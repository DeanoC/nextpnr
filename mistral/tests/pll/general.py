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


def cmux_selectors(text):
    """Extract lane-independent (CMUX_block, INPUT_SEL_value) pairs and check CMUXHG enables.

    Quartus chooses its own global clock lane numbers, so lane indices may
    differ between tools, but each pinned FPLL site/counter output must select
    the exact same CMUXHG/CMUXVG block and PLLIN INPUT_SEL code, and every
    active CMUXHG lane must set TESTSYN_ENOUT_SELECT to PRE_SYNENB.
    """
    inputs = {}
    enables = {}
    for line in text.splitlines():
        if not re.match(r"^[si] CMUX[HV]G\.", line):
            continue
        m = re.match(r"^s (CMUX[HV]G\.\d+\.\d+):INPUT_SEL\.(\d+) (\S+)$", line)
        if m:
            key = (m.group(1), int(m.group(2)))
            assert key not in inputs, f"duplicate CMUX INPUT_SEL: {line}"
            inputs[key] = m.group(3)
            continue
        m = re.match(r"^s (CMUX[HV]G\.\d+\.\d+):TESTSYN_ENOUT_SELECT\.(\d+) (\S+)$", line)
        if m:
            key = (m.group(1), int(m.group(2)))
            assert key not in enables, f"duplicate CMUX TESTSYN_ENOUT_SELECT: {line}"
            enables[key] = m.group(3)
            continue
        raise AssertionError(f"unexpected CMUX setting line: {line}")
    expected_enables = {key: "PRE_SYNENB" for key in inputs if key[0].startswith("CMUXHG.")}
    if enables != expected_enables:
        raise AssertionError(f"CMUX TESTSYN_ENOUT_SELECT mismatch: got {enables!r}, expected {expected_enables!r}")
    return sorted((block, sel) for (block, _lane), sel in inputs.items())


# FPLL (0, 14) counters C5 and C6 physically reach both CMUXHG (0, 35) and
# CMUXVG (42, 0). Quartus and nextpnr may pick opposite mux blocks for those
# two counters, so normalize them when comparing against the Quartus decode
# while also checking each fixture's exact CMUX selection below.
DUAL_HOMED_CMUX = {
    ("CMUXVG.042.000", "10"): ("CMUXHG.000.035", "17"),  # FPLL.000.014 C5
    ("CMUXVG.042.000", "11"): ("CMUXHG.000.035", "16"),  # FPLL.000.014 C6
}

EXPECTED_CMUX = {
    "eight": [
        ("CMUXHG.089.035", "0c"), ("CMUXHG.089.035", "0d"),
        ("CMUXHG.089.035", "0e"), ("CMUXHG.089.035", "0f"),
        ("CMUXVG.042.000", "0c"), ("CMUXVG.042.000", "0d"),
        ("CMUXVG.042.000", "0e"), ("CMUXVG.042.000", "0f"),
    ],
    "mister3": [
        ("CMUXHG.000.035", "0e"), ("CMUXHG.000.035", "16"),
        ("CMUXVG.042.000", "0d"), ("CMUXVG.042.000", "0e"),
        ("CMUXVG.042.000", "0f"),
    ],
    "oddn": [
        ("CMUXHG.000.035", "14"), ("CMUXHG.000.035", "15"),
        ("CMUXHG.000.035", "16"), ("CMUXHG.000.035", "17"),
    ],
    "refs": [
        ("CMUXHG.000.035", "0a"), ("CMUXHG.000.035", "0d"),
        ("CMUXHG.000.035", "0e"), ("CMUXHG.000.035", "16"),
    ],
}


def canonical_cmux(selectors):
    return sorted(DUAL_HOMED_CMUX.get(entry, entry) for entry in selectors)


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
        quartus_bt = (out / "quartus.bt").read_text()
        fixture_settings = (fixture / "pll-settings.txt").read_text()
        quartus = fpll_lines(quartus_bt, sites)
        assert quartus == fpll_lines(fixture_settings, sites), case
        quartus_cmux = cmux_selectors(quartus_bt)
        assert quartus_cmux == cmux_selectors(fixture_settings), case

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
        ours_bt = (out / "top.bt").read_text()
        ours = fpll_lines(ours_bt, sites)
        if ours != quartus:
            raise AssertionError("%s FPLL settings differ\n  quartus only: %s\n  nextpnr only: %s" % (
                case, sorted(quartus - ours), sorted(ours - quartus)))
        ours_cmux = cmux_selectors(ours_bt)
        if canonical_cmux(ours_cmux) != canonical_cmux(quartus_cmux):
            raise AssertionError("%s CMUX sources differ from Quartus\n  quartus: %s\n  nextpnr: %s" % (
                case, quartus_cmux, ours_cmux))
        if ours_cmux != EXPECTED_CMUX[case]:
            raise AssertionError("%s CMUX selectors differ from expected\n  expected: %s\n  nextpnr: %s" % (
                case, EXPECTED_CMUX[case], ours_cmux))
        print("PASS %s: %d PLLs, %d FPLL and %d CMUX settings verified against Quartus; RBF sha256 %s" % (
            case, len(placement), len(ours), len(ours_cmux),
            hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest()))


if __name__ == "__main__":
    main()
