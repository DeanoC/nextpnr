#!/usr/bin/env python3
"""Route fractional-N rates calculated outside the checked compatibility profiles."""

import argparse
import hashlib
import json
from pathlib import Path

from check import run
from fractional import ORACLE as SINGLE_ORACLE, settings as single_settings
from fractional_dual import ORACLE as DUAL_ORACLE, settings as dual_settings


def prepare_fixture(fixture, out):
    for name in ("pll_meter.v", "diagnostic.qsf", "clocks.sdc"):
        (out / name).write_text((fixture / name).read_text())


def route(yosys, nextpnr, mistral_cv, source, out, expected, clocks):
    run([str(yosys), "-p", f'read_verilog "{source.parent / "pll_meter.v"}" "{source}"; '
         'synth_intel_alm -nobram -nolutram -nodsp -top top; '
         f'write_json "{out / "synth.json"}"'], out / "yosys.log")
    command = [str(nextpnr), "--device", "5CSEBA6U23I7",
               str("--qsf"), str(source.parent / "diagnostic.qsf"),
               "--sdc", str(source.parent / "clocks.sdc"), "--freq", "50",
               "--compress-rbf"]
    log = run(command + ["--json", str(out / "synth.json"), "--rbf", str(out / "top.rbf"),
                         "--report", str(out / "timing.json"), "--write", str(out / "routed.json")],
              out / "route.log")
    assert "fractional-N requested" in log
    report = json.loads((out / "timing.json").read_text())
    util = report["utilization"]
    assert util["altera_pll"] == {"used": 1, "available": 6}
    assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
    assert util.get("MISTRAL_M10K", {"used": 0})["used"] == 0
    run([str(mistral_cv), "decomp", "5CSEBA6U23I7", str(out / "top.rbf"), str(out / "top.bt")],
        out / "decomp.log")
    actual = single_settings((out / "top.bt").read_text()) if clocks == 1 else dual_settings((out / "top.bt").read_text())
    assert actual == expected, (actual, expected)
    assert report["fmax"]["meter.refclk"]["constraint"] == 50
    for index, frequency in enumerate(clocks):
        clock = report["fmax"][f"clocks[{index}]" if len(clocks) > 1 else "fractional_clock"]
        assert abs(clock["constraint"] - frequency) <= frequency * 0.00005
        assert clock["achieved"] >= frequency
    print(f"PASS: {source.stem} fractional calculator host checks", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)

    single_cases = {
        "27": ("27.0 MHz", "D727", 27000000, 15, 8, "1999999a"),
        "99": ("99.0 MHz", "D799", 99000000, 5, 9, "e6666666"),
    }
    for name, (frequency, signature, hz, counter, multiplier, fraction) in single_cases.items():
        out = root / f"single-{name}"
        out.mkdir(parents=True, exist_ok=True)
        prepare_fixture(fixture, out)
        source = out / "fractional.v"
        source.write_text((fixture / "fractional.v").read_text()
                          .replace("12.288 MHz", frequency).replace("D715", signature))
        expected = dict(SINGLE_ORACLE)
        expected.update({"DPRIO0_CNT_HI_DIV.6": f"{(counter + 1) // 2:02x}",
                         "DPRIO0_CNT_LO_DIV.6": f"{counter // 2:02x}",
                         "FRACTIONAL_DIVISION_SETTING": fraction,
                         "M_CNT_HI_DIV_SETTING": f"{(multiplier + 1) // 2:02x}",
                         "M_CNT_LO_DIV_SETTING": f"{multiplier // 2:02x}"})
        if multiplier & 1:
            expected["M_CNT_ODD_DIV_DUTY_EN"] = "1"
        route(args.yosys.resolve(), args.nextpnr.resolve(), args.mistral_cv.resolve(),
              source, out, expected, [hz / 1e6])

    out = root / "dual-27-13.5"
    out.mkdir(parents=True, exist_ok=True)
    prepare_fixture(fixture, out)
    source = out / "fractional_dual.v"
    source.write_text((fixture / "fractional_dual.v").read_text()
                      .replace("12.288 MHz", "27.0 MHz")
                      .replace("24.576 MHz", "13.5 MHz")
                      .replace("D717", "D727"))
    expected = dict(DUAL_ORACLE)
    expected.update({"DPRIO0_CNT_HI_DIV.6": "08", "DPRIO0_CNT_LO_DIV.6": "07",
                     "DPRIO0_CNT_HI_DIV.7": "0f", "DPRIO0_CNT_LO_DIV.7": "0f",
                     "DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.6": "1",
                     "FRACTIONAL_DIVISION_SETTING": "1999999a"})
    expected.pop("DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.7", None)
    route(args.yosys.resolve(), args.nextpnr.resolve(), args.mistral_cv.resolve(),
          source, out, expected, [27.0, 13.5])


if __name__ == "__main__":
    main()
