#!/usr/bin/env python3
"""Check one PLL driving a compatible integer clock pair; host checks only, never programs hardware."""
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
    parser.add_argument("--mhz0", type=int, default=25)
    parser.add_argument("--mhz1", type=int, default=40)
    parser.add_argument("--skip-negative", action="store_true")
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "pll_meter.v"}" "{fixture / "dual.v"}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    design = json.loads((out / "synth.json").read_text())
    params = design["modules"]["top"]["cells"]["pll"]["parameters"]
    params["output_clock_frequency0"] = f"{args.mhz0}.0 MHz"
    params["output_clock_frequency1"] = f"{args.mhz1}.0 MHz"
    (out / "synth.json").write_text(json.dumps(design))
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
    for name, frequency in (("clocks[0]", args.mhz0), ("clocks[1]", args.mhz1), ("meter.refclk", 50)):
        clock = report["fmax"][name]
        assert abs(clock["constraint"] - frequency) <= frequency * 0.00005
        assert clock["achieved"] >= frequency
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "top.rbf"), str(out / "top.bt")], out / "decomp.log")
    bt = (out / "top.bt").read_text()
    assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 1
    settings = dict(re.findall(r"^s FPLL\.000\.014:(\S+) (\S+)$", bt, re.M))
    vco = next(v for v in (300, 320, 400) if v % args.mhz0 == 0 and v % args.mhz1 == 0)
    m, n, bw, cp, preset, phase = {300: (12, 2, 7, 1, 1, 0),
                                  320: (32, 5, 6, 2, 4, 2),
                                  400: (16, 2, 7, 1, 1, 0)}[vco]
    c0, c1 = vco // args.mhz0, vco // args.mhz1
    for name, value in {"M_CNT_HI_DIV_SETTING": (m + 1) // 2, "M_CNT_LO_DIV_SETTING": m // 2,
                        "N_CNT_HI_DIV_SETTING": (n + 1) // 2, "N_CNT_LO_DIV_SETTING": n // 2,
                        "DPRIO0_CNT_HI_DIV.6": (c0 + 1) // 2, "DPRIO0_CNT_LO_DIV.6": c0 // 2,
                        "DPRIO0_CNT_HI_DIV.7": (c1 + 1) // 2, "DPRIO0_CNT_LO_DIV.7": c1 // 2,
                        "M_CNT_LO_PRESET_SETTING": preset}.items():
        assert int(settings.get(name, "01"), 16) == value, (name, settings)
    for name, value in {"M_CNT_PH_MUX_PRESET_SETTING": phase, "N_CNT_ODD_DIV_DUTY_EN": 0,
                        "DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6": c0 % 2,
                        "DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.7": c1 % 2,
                        "BWCTRL": bw, "CP_CURRENT": cp}.items():
        default = "2" if name == "CP_CURRENT" else "0"
        assert int(settings.get(name, default)) == value, (name, settings)
    for name, value in {"C6_COUT_EN": "1", "C7_COUT_EN": "1", "CNT_IN_SRC.6": "0",
                        "CNT_IN_SRC.7": "0", "CLKIN_0_SRC": "4", "FBCLK_MUX_2": "1",
                        "VCO_DIV": "0"}.items():
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
        ("unsupported-pair", "output_clock_frequency1", "7.0 MHz", "unsupported dual PLL frequencies"),
        ("incompatible-pair", "output_clock_frequency1", "32.0 MHz", "unsupported dual PLL frequencies"),
        ("phase1", "phase_shift1", "100 ps", "unsupported parameter"),
    ):
        if args.skip_negative:
            continue
        invalid = copy.deepcopy(design)
        params = invalid["modules"]["top"]["cells"]["pll"]["parameters"]
        if name == "incompatible-pair":
            params["output_clock_frequency0"] = "25.0 MHz"
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
