#!/usr/bin/env python3
"""Check one PLL driving 25/40 MHz clocks; host checks only, never programs hardware."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re

from check import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "pll_meter.v"}" "{fixture / "dual.v"}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    assert sum(c["type"] == "altera_pll" for c in design["modules"]["top"]["cells"].values()) == 1
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
               "--freq", "50", "--compress-rbf"]
    run(command + ["--json", str(out / "synth.json"), "--rbf", str(out / "top.rbf"),
                   "--report", str(out / "timing.json"), "--write", str(out / "routed.json")], out / "route.log")
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["MISTRAL_CLKENA"]["used"] == 3
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
        assert util.get(kind, {"used": 0})["used"] == 0
    for name, frequency in (("clocks[0]", 25), ("clocks[1]", 40), ("meter.refclk", 50)):
        clock = report["fmax"][name]
        assert abs(clock["constraint"] - frequency) <= frequency * 0.00005
        assert clock["achieved"] >= frequency
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    bt = (out / "top.bt").read_text()
    assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 1
    settings = dict(re.findall(r"^s FPLL\.000\.014:(\S+) (\S+)$", bt, re.M))
    for name, value in {"M_CNT_HI_DIV_SETTING": 8, "M_CNT_LO_DIV_SETTING": 8,
                        "N_CNT_HI_DIV_SETTING": 1, "N_CNT_LO_DIV_SETTING": 1,
                        "DPRIO0_CNT_HI_DIV.6": 8, "DPRIO0_CNT_LO_DIV.6": 8,
                        "DPRIO0_CNT_HI_DIV.7": 5, "DPRIO0_CNT_LO_DIV.7": 5,
                        "M_CNT_LO_PRESET_SETTING": 1}.items():
        assert int(settings.get(name, "01"), 16) == value, (name, settings)
    for name in ("M_CNT_PH_MUX_PRESET_SETTING", "N_CNT_ODD_DIV_DUTY_EN",
                 "DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6", "DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.7"):
        assert settings.get(name, "0") == "0", (name, settings)
    for name, value in {"C6_COUT_EN": "1", "C7_COUT_EN": "1", "CNT_IN_SRC.6": "0",
                        "CNT_IN_SRC.7": "0", "BWCTRL": "7", "CP_CURRENT": "1",
                        "CLKIN_0_SRC": "4", "FBCLK_MUX_2": "1", "VCO_DIV": "0"}.items():
        assert settings[name] == value, (name, settings)
    for line in ("s CMUXHG.000.035:INPUT_SEL.2 16", "s CMUXHG.000.035:INPUT_SEL.3 15",
                 "s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.2 PRE_SYNENB",
                 "s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.3 PRE_SYNENB",
                 "s FPLL.000.073:PL_AUX_BG_POWERDOWN 1", "o OPT_B ffffff40.2dffffff"):
        assert line in bt, line
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]
    assert routed["cells"]["pll"]["connections"]["rst"], "driven reset was removed"
    assert "i FPLL.000.014:NRESET0 1" not in bt
    assert any(line.startswith("r ") and "FPLL.000.014:NRESET0" in line for line in bt.splitlines())
    for name, parameter, value, expected in (
        ("missing-frequency1", "output_clock_frequency1", None, "explicit output_clock_frequency1 is required"),
        ("three-outputs", "number_of_clocks", format(3, "032b"), "number_of_clocks must be 1 or 2"),
        ("unsupported-pair", "output_clock_frequency1", "30.0 MHz", "unsupported dual PLL frequencies"),
        ("phase1", "phase_shift1", "100 ps", "unsupported parameter"),
    ):
        invalid = copy.deepcopy(design)
        params = invalid["modules"]["top"]["cells"]["pll"]["parameters"]
        if value is None:
            del params[parameter]
        else:
            params[parameter] = value
        path = out / f"invalid-{name}.json"
        path.write_text(json.dumps(invalid))
        log = run(command + ["--json", str(path)], out / f"invalid-{name}.log", success=False)
        assert expected in log, log
    print("PASS: dual PLL host checks", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
