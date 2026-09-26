#!/usr/bin/env python3
"""Build and inspect the configurable single-output PLL fixture; never programs hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--mhz", type=int, required=True)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def run(command, name):
        with (out / name).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    source = out / "frequency.v"
    source.write_text((fixture / "reset.v").read_text()
                      .replace('"25.0 MHz"', f'"{args.mhz}.0 MHz"')
                      .replace("clk25", "pll_clock").replace("16'hD712", "16'hD713"))
    run([str(args.yosys.resolve()), "-p",
         f'read_verilog "{fixture / "pll_meter.v"}" "{source}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], "yosys.log")
    run([str(args.nextpnr.resolve()), "--json", str(out / "synth.json"), "--device", "5CSEBA6U23I7",
         "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(fixture / "clocks.sdc"),
         "--freq", "50", "--compress-rbf", "--rbf", str(out / "top.rbf"),
         "--write", str(out / "routed.json"), "--report", str(out / "timing.json")], "route.log")
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    assert util["MISTRAL_CLKENA"]["used"] == 2
    assert util["MISTRAL_MUL9X9"]["used"] == util["MISTRAL_M10K"]["used"] == 0
    assert util.get("MISTRAL_MLAB", {"used": 0})["used"] == 0
    for clock, frequency in (("meter.testclk", args.mhz), ("meter.refclk", 50)):
        assert abs(report["fmax"][clock]["constraint"] - frequency) <= frequency * 0.00005
        assert report["fmax"][clock]["achieved"] >= frequency
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "top.rbf"), str(out / "top.bt")], "decomp.log")
    bt = (out / "top.bt").read_text()
    routed = json.loads((out / "routed.json").read_text())["modules"]["top"]
    pll = routed["cells"]["pll"]
    assert pll["connections"]["rst"], "driven reset was removed"
    assert "FPLL.000.014:NRESET0" in bt
    assert "i FPLL.000.014:NRESET0 1" not in bt
    assert any(line.startswith("r ") and "FPLL.000.014:NRESET0" in line for line in bt.splitlines()), "reset is not routed"
    assert "s FPLL.000.014:FPLL_ENABLE 1" in bt
    assert "s FPLL.000.014:FBCLK_MUX_2 1" in bt
    assert "s FPLL.000.073:PL_AUX_BG_POWERDOWN 1" in bt
    assert "s CMUXHG.000.035:INPUT_SEL.2 16" in bt
    assert "o OPT_B ffffff40.2dffffff" in bt
    # Check actual emitted counters and analog settings against the oracle tuples.
    settings = {}
    for line in bt.splitlines():
        if line.startswith("s FPLL.000.014:"):
            name, value = line.split()[1:]
            settings[name.split(":")[1]] = value
    profiles = ((12, 2, 300, 7, 1, 1, 0),
                (32, 5, 320, 6, 2, 4, 2),
                (52, 5, 520, 4, 2, 6, 2))
    selected = next((profile for profile in profiles
                     if profile[2] % args.mhz == 0 and 2 <= profile[2] // args.mhz <= 512), None)
    assert selected is not None, args.mhz
    m, n, vco, expected_bw, expected_cp, expected_low, expected_phase = selected
    c = vco // args.mhz
    for name, value in {
        "M_CNT_HI_DIV_SETTING": (m + 1) // 2, "M_CNT_LO_DIV_SETTING": m // 2,
        "N_CNT_HI_DIV_SETTING": (n + 1) // 2, "N_CNT_LO_DIV_SETTING": n // 2,
        "DPRIO0_CNT_HI_DIV.6": (c + 1) // 2, "DPRIO0_CNT_LO_DIV.6": c // 2,
    }.items():
        assert int(settings.get(name, "01"), 16) == value, (name, settings)
    assert int(settings.get("DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6", "0")) == c % 2
    assert settings.get("N_CNT_ODD_DIV_DUTY_EN", "0") == "0"
    assert int(settings.get("BWCTRL", "4")) == expected_bw
    assert int(settings.get("CP_CURRENT", "2")) == expected_cp
    assert int(settings.get("M_CNT_LO_PRESET_SETTING", "01"), 16) == expected_low
    assert int(settings.get("M_CNT_PH_MUX_PRESET_SETTING", "0")) == expected_phase
    print("PASS: frequency host checks", args.mhz, report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
