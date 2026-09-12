#!/usr/bin/env python3
"""Route and inspect the checked fractional-N profile; never programs hardware."""
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
CLKIN_0_SRC 4
CNT_IN_SRC.6 0
CTRL_OVERRIDE_SETTING 0
DPRIO0_CNT_HI_DIV.6 11
DPRIO0_CNT_LO_DIV.6 10
DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6 1
DSM_OUT_SEL 1
FBCLK_MUX_2 1
FPLL_ENABLE 1
FRACTIONAL_DIVISION_SETTING 1c2e33f0
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
    parser.add_argument("--mhz", choices=("12.288", "11.2896", "74.25"), default="12.288")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = fixture / "fractional.v"
    oracle = dict(ORACLE)
    if args.mhz == "11.2896":
        source = out / "fractional.v"
        source.write_text((fixture / "fractional.v").read_text()
                          .replace("12.288 MHz", "11.2896 MHz").replace("D715", "D716"))
        oracle.update({"DPRIO0_CNT_HI_DIV.6": "12", "DPRIO0_CNT_LO_DIV.6": "12",
                       "FRACTIONAL_DIVISION_SETTING": "20e6293f"})
        del oracle["DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6"]
    if args.mhz == "74.25":
        source = out / "fractional.v"
        source.write_text((fixture / "fractional.v").read_text()
                          .replace("12.288 MHz", "74.25 MHz").replace("D715", "D742"))
        oracle.update({"DPRIO0_CNT_HI_DIV.6": "03", "DPRIO0_CNT_LO_DIV.6": "03",
                       "FRACTIONAL_DIVISION_SETTING": "e8f5c239"})
        del oracle["DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6"]
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
    assert util["MISTRAL_CLKENA"]["used"] == 2
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
        assert util.get(kind, {"used": 0})["used"] == 0
    fractional_word = int(oracle["FRACTIONAL_DIVISION_SETTING"], 16)
    divider = sum(int(oracle[f"DPRIO0_CNT_{half}_DIV.6"], 16) for half in ("HI", "LO"))
    achieved_hz = Fraction(50_000_000) * (8 + Fraction(fractional_word, 2**32)) / divider
    error_ppm = (achieved_hz / (Fraction(args.mhz) * 1_000_000) - 1) * 1_000_000
    if args.mhz == "12.288":
        assert 0 < error_ppm < Fraction(2, 1_000_000)
    elif args.mhz == "11.2896":
        # Quartus uses this checked word, not the mathematically nearest word.
        assert Fraction(-247, 100_000) < error_ppm < Fraction(-246, 100_000)
    else:
        assert Fraction(-2257, 1_000_000) < error_ppm < Fraction(-2256, 1_000_000)
    for name, mhz in (("fractional_clock", achieved_hz / 1_000_000), ("meter.refclk", 50)):
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
    assert "s CMUXHG.000.035:INPUT_SEL.2 16" in bt
    assert "o OPT_B ffffff40.2dffffff" in bt
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]
    assert routed["cells"]["pll"]["connections"]["rst"], "driven reset removed"
    assert "i FPLL.000.014:NRESET0 1" not in bt
    assert any(line.startswith("r ") and "FPLL.000.014:NRESET0" in line for line in bt.splitlines())
    for name, parameter, value, expected in (
        ("reference", "reference_clock_frequency", "25.0 MHz", "fractional-N selector requires a 50 MHz reference"),
        ("output", "output_clock_frequency0", "0.5 MHz", "fractional-N selector requires a 50 MHz reference"),
        ("integer-mode", "fractional_vco_multiplier", "false", "unsupported PLL output frequency"),
        ("two-outputs", "number_of_clocks", f"{2:032b}", "explicit output_clock_frequency1 is required"),
    ):
        invalid = copy.deepcopy(design)
        invalid["modules"]["top"]["cells"]["pll"]["parameters"][parameter] = value
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    print(f"PASS: fractional-N host checks; calculated achieved {float(achieved_hz):.9f} Hz, error {float(error_ppm):.9g} ppm")
    print("Timing", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
