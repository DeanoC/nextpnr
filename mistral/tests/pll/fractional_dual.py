#!/usr/bin/env python3
"""Route and inspect the checked dual-output fractional-N profile; never programs hardware."""
import argparse
import copy
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re

from check import run


# Complete non-default selected-FPLL settings from the Quartus 17.0.2 oracle.
# CP_CURRENT=2 and M presets 1/0 are defaults and therefore absent in decomp.
ORACLE = dict(line.split() for line in """
BWCTRL 7
C6_COUT_EN 1
C7_COUT_EN 1
CLKIN_0_SRC 4
CNT_IN_SRC.6 0
CNT_IN_SRC.7 0
CTRL_OVERRIDE_SETTING 0
DPRIO0_CNT_HI_DIV.6 11
DPRIO0_CNT_HI_DIV.7 09
DPRIO0_CNT_LO_DIV.6 11
DPRIO0_CNT_LO_DIV.7 08
DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.7 1
DSM_OUT_SEL 1
FBCLK_MUX_2 1
FPLL_ENABLE 1
FRACTIONAL_DIVISION_SETTING 5b18548b
LOCK_FILTER_CFG_SETTING 019
M_CNT_HI_DIV_SETTING 04
M_CNT_LO_DIV_SETTING 04
N_CNT_BYPASS_EN 1
N_CNT_HI_DIV_SETTING 00
N_CNT_LO_DIV_SETTING 00
NREVERT_INVERT 1
TCLK_SEL 0
UNLOCK_FILTER_CFG_SETTING 2
VCO0PH_EN 1
VCO_DIV 0
VCO_PH0_EN 1
VCO_PH1_EN 1
VCO_PH2_EN 1
VCO_PH3_EN 1
VCO_PH4_EN 1
VCO_PH5_EN 1
VCO_PH6_EN 1
VCO_PH7_EN 1
""".strip().splitlines())


def settings(bt):
    return dict(re.findall(r"^s FPLL\.000\.014:(\S+) (\S+)$", bt, re.M))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--oracle-bt", type=Path,
                        help="also compare against a local Quartus 17.0.2 decomp")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = fixture / "fractional_dual.v"
    oracle = dict(ORACLE)
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "pll_meter.v"}" "{source}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    assert sum(c["type"] == "altera_pll" for c in design["modules"]["top"]["cells"].values()) == 1
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
               "--freq", "50", "--compress-rbf"]
    log = run(command + ["--json", str(out / "synth.json"), "--rbf", str(out / "top.rbf"),
                         "--report", str(out / "timing.json"), "--write", str(out / "routed.json")], out / "route.log")
    assert "fractional-N requested" in log and "error" in log
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["MISTRAL_CLKENA"]["used"] == 3
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
        assert util.get(kind, {"used": 0})["used"] == 0
    fractional_word = int(oracle["FRACTIONAL_DIVISION_SETTING"], 16)
    achieved = [Fraction(50_000_000) * (8 + Fraction(fractional_word, 2**32)) / c for c in (34, 17)]
    errors = [(hz / requested - 1) * 1_000_000 for hz, requested in zip(achieved, (12_288_000, 24_576_000))]
    assert errors[0] == errors[1]
    assert abs(errors[0]) < Fraction(12, 1_000_000)
    for name, mhz in (("clocks[0]", achieved[0] / 1_000_000),
                      ("clocks[1]", achieved[1] / 1_000_000), ("meter.refclk", 50)):
        clock = report["fmax"][name]
        assert abs(clock["constraint"] - float(mhz)) <= float(mhz) * 0.00005
        assert clock["achieved"] >= float(mhz)
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    bt = (out / "top.bt").read_text()
    emitted = settings(bt)
    assert emitted == oracle, (emitted, oracle)
    if args.oracle_bt:
        assert emitted == settings(args.oracle_bt.read_text())
    assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 1
    assert "s FPLL.000.073:PL_AUX_BG_POWERDOWN 1" in bt
    for line in ("s CMUXHG.000.035:INPUT_SEL.2 16", "s CMUXHG.000.035:INPUT_SEL.3 15",
                 "s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.2 PRE_SYNENB",
                 "s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.3 PRE_SYNENB"):
        assert line in bt, line
    assert "o OPT_B ffffff40.2dffffff" in bt
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]
    assert routed["cells"]["pll"]["connections"]["rst"], "driven reset removed"
    assert "i FPLL.000.014:NRESET0 1" not in bt
    assert any(line.startswith("r ") and "FPLL.000.014:NRESET0" in line for line in bt.splitlines())
    for name, changes, expected in (
        ("swapped-pair", {"output_clock_frequency0": "24.576 MHz", "output_clock_frequency1": "12.288 MHz"},
         "fractional-N dual profile requires"),
        ("bad-second", {"output_clock_frequency1": "24.0 MHz"}, "fractional-N dual profile requires"),
        ("reference", {"reference_clock_frequency": "25.0 MHz"}, "fractional-N dual profile requires"),
        ("integer-mode", {"fractional_vco_multiplier": "false"}, "unsupported dual PLL frequencies"),
        ("three-outputs", {"number_of_clocks": f"{3:032b}"}, "number_of_clocks must be 1 or 2"),
        ("phase1", {"phase_shift1": "100 ps"}, "unsupported parameter"),
    ):
        invalid = copy.deepcopy(design)
        invalid["modules"]["top"]["cells"]["pll"]["parameters"].update(changes)
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    print("PASS: dual fractional-N host checks; achieved Hz", [float(x) for x in achieved],
          "error ppm", [float(x) for x in errors])
    print("Timing", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
